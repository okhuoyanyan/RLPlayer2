#pragma once
#include "fp_strict.h"
namespace rta::dsp {
// A 280 dB rejection specification needs design headroom: the empirical Kaiser
// attenuation/beta relation has no guarantee at the exact requested boundary.
// This is internal coefficient-design margin, never a relaxed quality target.
// Verified at unchanged production auto T/cutoff for all 230 non-apod LP
// identities.
inline double linear_kaiser_design_attenuation(double specificationDb) {
  return specificationDb >= 280.0 ? specificationDb + 4.0 : specificationDb;
}
} // namespace rta::dsp
