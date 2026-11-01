// Apodizing quality tap floor. This changes support, never declared F1/F2.
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rta::dsp {
// Independent of WindowKind so this header can be included before its definition.
// gaussian=false covers the production high-attenuation Kaiser/BH branch.
inline int apod_quality_taps(double f1, double f2, bool gaussian,
                             double attenDb, int requested = 0) {
    if (!std::isfinite(f1) || !std::isfinite(f2) ||
        !(f1 > 0.0 && f2 > f1 && f2 <= 1.0) || !std::isfinite(attenDb))
        throw std::invalid_argument("invalid apodizing F1/F2 or attenuation");
    // At the raised-cosine edge, the finite window rounds the curvature.
    // Leading edge error is proportional to window curvature/(T*(F2-F1))^2.
    // These products target ~0.005 dB, rather than the first 0.010 dB crossing.
    // Gaussian alpha follows make_window; Kaiser beta follows kaiser.h.
    const double alpha = 3.0 + 0.011 * std::max(0.0, attenDb - 60.0);
    const double beta = 0.1102 * std::max(0.0, attenDb - 8.7);
    const double product = gaussian ? 110.0 * alpha / 3.66
                                    : 160.0 * std::sqrt(std::max(1.0, beta) / 29.89726);
    const double need = std::ceil(product / (f2 - f1));
    if (need > 10000000.0) throw std::length_error("apodizing quality support too large");
    return std::max(requested, static_cast<int>(need));
}
}
