// High-precision finite Kaiser/sinc DESIGN only. Output stays binary64.
// No Python/runtime DLL dependency; strict two-double arithmetic + explicit FMA.
#pragma once
#include "fp_strict.h"
#include "design_control.h"
#include <cmath>
#include <cfenv>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace rta::dsp::coefficient_precision {
struct DD { double hi=0,lo=0; };
inline DD norm(double a,double b){double s=a+b,v=s-a;return {s,(a-(s-v))+(b-v)};}
inline DD add(DD a,DD b){double s=a.hi+b.hi,v=s-a.hi,e=(a.hi-(s-v))+(b.hi-v);return norm(s,e+a.lo+b.lo);}
inline DD neg(DD x){return {-x.hi,-x.lo};}
inline DD sub(DD a,DD b){return add(a,neg(b));}
inline DD mul(DD a,DD b){double p=a.hi*b.hi;double e=std::fma(a.hi,b.hi,-p);e+=a.hi*b.lo+a.lo*b.hi;e+=a.lo*b.lo;return norm(p,e);}
inline DD div(DD a,DD b){
 if(b.hi==0)throw std::domain_error("DD division by zero");
 DD q{a.hi/b.hi,0};for(int k=0;k<2;++k){DD r=sub(a,mul(b,q));q=add(q,{(r.hi+r.lo)/b.hi,0});}return q;
}
inline DD sqrt(DD a){
 if(a.hi<0)throw std::domain_error("DD square root of negative");if(a.hi==0&&a.lo==0)return {};
 DD q{std::sqrt(a.hi),0};for(int k=0;k<2;++k)q=add(q,div(sub(a,mul(q,q)),mul({2,0},q)));return q;
}
inline constexpr DD pi{3.14159265358979311600,1.22464679914735320717e-16};
inline DD sinpi(DD x){
 const double integer=std::nearbyint(x.hi);DD r=sub(x,{integer,0});DD angle=mul(pi,r),square=mul(angle,angle),term=angle,result=angle;
 for(int k=1;k<40;++k){term=neg(div(mul(term,square),{double((2*k)*(2*k+1)),0}));result=add(result,term);if(std::abs(term.hi)<=std::abs(result.hi)*1e-34)break;}
 if(std::fmod(std::abs(integer),2.)==1.)result=neg(result);return result;
}
inline DD bessel_i0(DD x){
 DD z=div(mul(x,x),{4,0}),term{1,0},sum{1,0};
 for(int k=1;k<256;++k){term=div(mul(term,z),{double(k*k),0});sum=add(sum,term);if(std::abs(term.hi)<=std::abs(sum.hi)*1e-34)return sum;}
 throw std::runtime_error("DD I0 design series did not converge");
}
inline DD sinc(double cutoff,double x){if(x==0)return {cutoff,0};return div(sinpi(mul({cutoff,0},{x,0})),mul(pi,{x,0}));}
inline DD window(int n,int i,double beta,bool zeroEndpoints=true){
 if(n<1||i<0||i>=n||!std::isfinite(beta)||beta<0||beta>100)throw std::invalid_argument("Invalid DD Kaiser window arguments");
 if(n==1)return {1,0};if(zeroEndpoints&&(i==0||i==n-1))return {};
 DD r=div({double(2ll*i-(n-1)),0},{double(n-1),0});DD a=sub({1,0},mul(r,r));
 return div(bessel_i0(mul({beta,0},sqrt(a))),bessel_i0({beta,0}));
}
// referenceGain=1 reproduces the normalized prototype contract. Supplying the
// actual interpolation L retains precision until ONE final raw-coefficient round.
inline std::vector<double> lowpass(int n,double cutoff,double beta,double referenceGain=1,bool zeroEndpoints=true){
 check_design_cancel();
 if(std::fegetround()!=FE_TONEAREST)throw std::runtime_error("DD coefficient design requires FE_TONEAREST");
 if(n<3||!std::isfinite(cutoff)||cutoff<=0||cutoff>1||!std::isfinite(beta)||beta<0||beta>100||!std::isfinite(referenceGain)||referenceGain<=0)throw std::invalid_argument("Invalid DD FIR design arguments");
 std::vector<DD> kernel(static_cast<size_t>(n));const DD denominator=bessel_i0({beta,0});
 for(int i=0;i<n/2+n%2;++i){
  if((i&4095)==0)check_design_cancel();
  DD w{};
  if(!zeroEndpoints||i>0){DD r=div({double(2ll*i-(n-1)),0},{double(n-1),0});DD a=sub({1,0},mul(r,r));w=div(bessel_i0(mul({beta,0},sqrt(a))),denominator);}
  const double x=double(i)-double(n-1)*.5;const DD value=mul(sinc(cutoff,x),w);kernel[size_t(i)]=value;kernel[size_t(n-1-i)]=value;
 }
 DD sum{};for(const auto& v:kernel)sum=add(sum,v);if(sum.hi==0)throw std::runtime_error("DD FIR has zero normalization");
 std::vector<double> out(static_cast<size_t>(n));
 for(int i=0;i<n;++i){if((i&4095)==0)check_design_cancel();DD v=div(mul(kernel[size_t(i)],{referenceGain,0}),sum);out[size_t(i)]=v.hi+v.lo;}
 return out;
}
} // namespace rta::dsp::coefficient_precision
