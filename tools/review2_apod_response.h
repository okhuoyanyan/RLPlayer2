#pragma once
#include "dsp/filter_design.h"
#include <vector>
#include <cmath>
#include <limits>
namespace review2_apod {
struct StrictF1 { double db; bool pass; };
inline StrictF1 strict_f1(const std::vector<double>& h, double normalizedF1) {
    if (h.empty() || !std::isfinite(normalizedF1) || normalizedF1<=0 || normalizedF1>1)
        return {std::numeric_limits<double>::quiet_NaN(),false};
    for(double v:h) if(!std::isfinite(v)) return {std::numeric_limits<double>::quiet_NaN(),false};
#ifdef REVIEW2_APOD_MUTANT_WEAK_BOUNDARY
    normalizedF1 *= 0.9; // Negative mutation: this actual shared path MUST fail regression.
#endif
    const double db=rta::dsp::magnitude_db_exact(h,normalizedF1);
    return {db,std::isfinite(db) && std::abs(db)<=0.01};
}
inline int exit_status(int measured, int failures) { return measured<=0?2:failures?1:0; }
}
