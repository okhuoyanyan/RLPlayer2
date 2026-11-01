// Design-time double-double FFT convolution. Stored/output coefficients remain
// double. FFT root high/low parts were generated with mpmath at 90 decimal
// digits. A plain double FFT creates convolution leakage that can exceed a 280
// dB target.
#pragma once
#include "fp_strict.h"
#include <algorithm>
#include <cmath>
#include <vector>
namespace rta::dsp::phase_detail {
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
static const DC roots[] = {
    {{1, 0}, {4.666454797294691e-93, -7.6348764571822243e-110}},
    {{-1, 0}, {-2.3332273986473455e-93, 3.8174382285911121e-110}},
    {{1.1666136993236728e-93, -1.9087191142955561e-110}, {-1, 0}},
    {{0.70710678118654757, -4.8336466567264567e-17},
     {-0.70710678118654757, 4.8336466567264567e-17}},
    {{0.92387953251128674, 1.7645047084336677e-17},
     {-0.38268343236508978, 1.0050772696461588e-17}},
    {{0.98078528040323043, 1.8546939997825006e-17},
     {-0.19509032201612828, 7.9910790684617313e-18}},
    {{0.99518472667219693, -4.248691367830441e-17},
     {-0.098017140329560604, 1.634582362244256e-18}},
    {{0.99879545620517241, -1.2291693337075465e-17},
     {-0.049067674327418015, 6.7961037205182801e-19}},
    {{0.99969881869620425, -2.9851486403799753e-17},
     {-0.024541228522912288, 9.1868490125778782e-20}},
    {{0.9999247018391445, 3.7931082512668012e-17},
     {-0.012271538285719925, -6.919790764028317e-19}},
    {{0.99998117528260111, 3.3568103522895585e-17},
     {-0.0061358846491544753, -9.0545257482474933e-20}},
    {{0.99999529380957619, -1.9668064285322189e-17},
     {-0.0030679567629659761, -1.2690279085455925e-19}},
    {{0.99999882345170188, 3.0678817161261078e-17},
     {-0.0015339801862847657, 1.0467712971596958e-19}},
    {{0.99999970586288223, -7.4655147696415975e-18},
     {-0.00076699031874270449, -4.1438995560568478e-20}},
    {{0.99999992646571789, -4.0976445165762006e-17},
     {-0.00038349518757139556, -2.5865284466133177e-20}},
    {{0.99999998161642933, -4.044406916806987e-17},
     {-0.00019174759731070332, 1.119359730219912e-20}},
    {{0.99999999540410733, -2.0672132087728589e-17},
     {-9.5873799095977345e-05, -1.2012875748338165e-21}},
    {{0.99999999885102686, -3.358367831305141e-17},
     {-4.7936899603066881e-05, -3.2393955414258545e-21}},
    {{0.99999999971275666, 4.7073977298264975e-17},
     {-2.3968449808418219e-05, 5.8859069300696563e-22}},
    {{0.99999999992818922, -4.3745235303862389e-17},
     {-1.1984224905069707e-05, 5.7023870419370293e-22}},
    {{0.99999999998204725, 4.4574681255469053e-17},
     {-5.9921124526424275e-06, -3.156653355161829e-22}},
    {{0.99999999999551181, 1.1143660242003314e-17},
     {-2.9960562263346608e-06, 8.4706510983029793e-23}},
    {{0.99999999999887801, -5.2725236800248493e-17},
     {-1.4980281131690111e-06, -8.6148027605014136e-23}},
    {{0.99999999999971945, 4.2329841991852486e-17},
     {-7.4901405658471574e-07, 2.0272664527111588e-23}},
    {{0.99999999999992983, 3.8338036111133086e-17},
     {-3.7450702829238413e-07, 4.8197772511469996e-24}},
    {{0.99999999999998246, 9.584509027629587e-18},
     {-1.8725351414619535e-07, 1.7453192489360667e-24}},
};

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

inline std::vector<double> convolve(const std::vector<double> &a,
                                    const std::vector<double> &b) {
  if (a.empty() || b.empty())
    return {};
  size_t need = a.size() + b.size() - 1, n = 1;
  while (n < need)
    n *= 2;
  std::vector<DC> A(n), B(n);
  for (size_t i = 0; i < a.size(); ++i)
    A[i].r.h = a[i];
  for (size_t i = 0; i < b.size(); ++i)
    B[i].r.h = b[i];
  fft(A);
  fft(B);
  for (size_t i = 0; i < n; ++i) {
    A[i] = mul(A[i], B[i]);
    A[i].i = neg(A[i].i);
  }
  fft(A);
  std::vector<double> out(need);
  for (size_t i = 0; i < need; ++i)
    out[i] = (A[i].r.h + A[i].r.l) / (double)n;
  return out;
}
} // namespace rta::dsp::phase_detail
