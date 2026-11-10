#pragma once
#include "modulator.h"

namespace rta::dsp {

// Product-output admission, separate from the research modulator constructor.
// The 20/50 kHz fixed-corner designs have independently reproduced audio-band
// gain errors even with zero protection counters and 50-digit arithmetic.
// Raising precision does not repair that NTF. Reject the known-bad setting
// explicitly; never silently substitute another corner or modulator family.
// This lower bound is not a proof that every other arbitrary setting is safe.
inline void validate_one_bit_sdm_corner(ModulatorFamily family, bool ecCorrection,
                                        double cornerHz) {
    if (!std::isfinite(cornerHz) || cornerHz <= 0.0)
        throw std::invalid_argument("SDM corner must be finite and positive");
    // Match SdmModulator's designNtf dispatch: Chebyshev takes precedence over
    // ecCorrection, whereas the actual EC design does not use this corner.
    const bool actualEc = family == ModulatorFamily::Ec ||
                         (family != ModulatorFamily::Chebyshev && ecCorrection);
    if (!actualEc && cornerHz < 100000.0)
        throw std::invalid_argument(
            "SDM_CORNER_QUALITY: One-bit noise-shaping corners below 100 kHz can distort the audio band; use 0/default 100 kHz or the EC family.");
}

} // namespace rta::dsp
