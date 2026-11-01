// Two-component binary64 arithmetic for FIR response measurement only.
// Error-free TwoSum / FMA residual primitives; see Bailey et al.,
// https://www.davidhbailey.com/dhbpapers/qd.pdf .
// Calibrated on actual production coefficients with independent mpmath 90 dps;
// finite-point accuracy is not a proof of continuous-band extrema.
#pragma once
#include "fp_strict.h"
#include <cmath>
#include <vector>
#include <limits>
namespace rta::dsp::response_precision {
struct DD {
  double h = 0, l = 0;
};
inline DD norm(double h, double l) {
  double z = h + l;
  return {z, l - (z - h)};
}
inline DD add(DD a, DD b) {
  double s = a.h + b.h, bb = s - a.h, e = (a.h - (s - bb)) + (b.h - bb);
  return norm(s, e + a.l + b.l);
}
inline DD neg(DD a) { return {-a.h, -a.l}; }
inline DD mul(DD a, DD b) {
  double p = a.h * b.h, e = std::fma(a.h, b.h, -p);
  return norm(p, e + a.h * b.l + a.l * b.h);
}
struct DC {
  DD r, i;
};
inline DC add(DC a, DC b) { return {add(a.r, b.r), add(a.i, b.i)}; }
inline DC sub(DC a, DC b) { return {add(a.r, neg(b.r)), add(a.i, neg(b.i))}; }
inline DC mul(DC a, DC b) {
  return {add(mul(a.r, b.r), neg(mul(a.i, b.i))),
          add(mul(a.r, b.i), mul(a.i, b.r))};
}
inline DD divd(DD a, double b) {
  double q = a.h / b;
  DD rem = add(a, neg(mul(DD{q, 0}, DD{b, 0})));
  return add(DD{q, 0}, DD{(rem.h + rem.l) / b, 0});
}
inline DC rotation(double hz, double fs) {
  DD x = divd(
         mul(DD{-6.2831853071795864769, -2.449293598294706414e-16}, DD{hz, 0}),
         fs),
     xx = mul(x, x), ct{1, 0}, st = x, c = ct, v = st;
  for (int k = 1; k < 28; ++k) {
    ct = divd(neg(mul(ct, xx)), (2. * k - 1) * (2. * k));
    st = divd(neg(mul(st, xx)), (2. * k) * (2. * k + 1));
    c = add(c, ct);
    v = add(v, st);
  }
  return {c, v};
}
inline double magnitude_db(const std::vector<double>& h, double freqNorm) {
  if(h.empty()||!std::isfinite(freqNorm)) return std::numeric_limits<double>::quiet_NaN();
  for(double x:h) if(!std::isfinite(x)) return std::numeric_limits<double>::quiet_NaN();
  // Periodicity preserves the public normalized-Nyquist frequency convention.
  const double f=std::remainder(freqNorm,2.0);
  if(f==0.0||std::abs(f)==1.0) {
    DD sum;const bool nyquist=std::abs(f)==1.0;
    // Pair mirror coefficients before accumulation. For an even symmetric
    // FIR, every Nyquist pair is exactly zero; no trigonometric residue is a
    // meaningful dB reference for this analytic zero.
    for(size_t i=0;i<h.size()/2;++i) {
      size_t j=h.size()-1-i;
      double a=nyquist&&(i&1)?-h[i]:h[i];
      double b=nyquist&&(j&1)?-h[j]:h[j];
      sum=add(sum,add(DD{a,0},DD{b,0}));
    }
    if(h.size()&1){size_t m=h.size()/2;sum=add(sum,DD{nyquist&&(m&1)?-h[m]:h[m],0});}
    if(!std::isfinite(sum.h)||!std::isfinite(sum.l))return std::numeric_limits<double>::quiet_NaN();
    double amplitude=std::abs(sum.h+sum.l);
    return 20.0*std::log10(amplitude>1e-300?amplitude:1e-300);
  }
  DC w=rotation(f,2.0),z{{1,0},{0,0}},sum;
  for(double x:h) {
    sum=add(sum,{mul({x,0},z.r),mul({x,0},z.i)});
    z=mul(z,w);
  }
  const double mag=std::hypot(sum.r.h+sum.r.l,sum.i.h+sum.i.l);
  if(!std::isfinite(mag))return std::numeric_limits<double>::quiet_NaN();
  return 20.0*std::log10(mag>1e-300?mag:1e-300);
}
}
