#pragma once
#include "review_response.h"
#include <stdexcept>
namespace dsp_measure {
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
#include "review2_spectrum_roots.h"
inline double mag(DC a) { return std::hypot(a.r.h + a.r.l, a.i.h + a.i.l); }
inline double db(double a) { return 20 * log10(std::max(a, 1e-300)); }
inline void fft(std::vector<DC> &a) {
  size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t b = n >> 1;
    for (; j & b; b >>= 1)
      j ^= b;
    j ^= b;
    if (i < j)
      std::swap(a[i], a[j]);
  }
  unsigned stage = 1;
  for (size_t len = 2; len <= n; len *= 2, ++stage) {
    std::vector<DC> w(len / 2);
    w[0] = {{1, 0}, {0, 0}};
    for (size_t k = 1; k < w.size(); ++k)
      w[k] = mul(w[k - 1], roots[stage]);
    for (size_t i = 0; i < n; i += len)
      for (size_t k = 0; k < len / 2; ++k) {
        DC u = a[i + k], v = mul(a[i + k + len / 2], w[k]);
        a[i + k] = add(u, v);
        a[i + k + len / 2] = sub(u, v);
      }
  }
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
inline double direct(const std::vector<double> &h, double hz, double fs) {
  DC w = rotation(hz, fs), z{{1, 0}, {0, 0}}, sum;
  for (double x : h) {
    sum = add(sum, {mul({x, 0}, z.r), mul({x, 0}, z.i)});
    z = mul(z, w);
  }
  return db(mag(sum));
}

}
