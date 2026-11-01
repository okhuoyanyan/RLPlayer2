#pragma once
#include "fp_strict.h"
#include <array>
#include <complex>
#include <cmath>
#include <algorithm>

namespace rta::dsp {
// Preserve the Butterworth factors instead of expanding a near-cancelling
// seventh-degree polynomial. Each section has unit direct feedthrough.
// Consequently NTF(e)-e at the current sample is the sum of the old z1
// states; it is strictly causal and does not introduce an extra sample delay.
struct SdmNtfSections {
    struct Coeff { double a1=0, a2=0, g1=0, g2=0, radius=0; int degree=0; };
    std::array<Coeff,4> c{};
    int count=0;
    static SdmNtfSections butterworth(int order, double cutoff, double fs) {
        SdmNtfSections out;
        if (order<1 || order>8 || !(fs>0) || !(cutoff>0 && cutoff<fs*.5)) return out;
        constexpr long double pi=3.141592653589793238462643383279502884L;
        using C=std::complex<long double>;
        const long double wc=std::tan(pi*cutoff/fs);
        if (order&1) {
            const long double p=(1-wc)/(1+wc);
            auto& s=out.c[out.count++];
            s.a1=double(-p); s.g1=double(p-1); s.radius=double(std::abs(p)); s.degree=1;
        }
        // Real pole first, then increasing Q. No root finding on rounded
        // expanded coefficients: those roots are ill-conditioned here.
        for (int k=order/2-1;k>=0;--k) {
            const long double theta=pi*(2*k+order+1)/(2*order);
            const C a=wc/C(std::cos(theta),std::sin(theta));
            const C p=(C(1)+a)/(C(1)-a);
            auto& s=out.c[out.count++];
            s.a1=double(-2*p.real()); s.a2=double(std::norm(p));
            s.g1=double(2*(p.real()-1)); s.g2=double(1-std::norm(p));
            s.radius=double(std::abs(p)); s.degree=2;
        }
        return out;
    }
    double max_radius() const {
        double r=0; for (int k=0;k<count;++k) r=std::max(r,c[k].radius); return r;
    }
    double gain(double freqNorm) const {
        constexpr double pi=3.141592653589793238462643383279502884;
        const double w=pi*freqNorm;
        const std::complex<double> z(std::cos(w),-std::sin(w));
        // expm1-like evaluation retains the DC zeros in diagnostics too.
        const std::complex<double> oneMinusZ(2*std::sin(w*.5)*std::sin(w*.5),std::sin(w));
        double v=1;
        for (int k=0;k<count;++k) {
            const auto& s=c[k];
            auto den=1.0+s.a1*z+s.a2*z*z;
            auto num=oneMinusZ;
            if (s.degree==2) num*=oneMinusZ;
            v*=std::abs(num)/std::max(std::abs(den),1e-300);
        }
        return v;
    }
};
struct SdmSectionState {
    std::array<double,4> z1{},z2{};
    void reset() { z1.fill(0); z2.fill(0); }
    double feedback(int count) const {
        double w=0; for (int k=0;k<count;++k) w+=z1[k]; return w;
    }
    void advance(const SdmNtfSections& sections, double e) {
        for (int k=0;k<sections.count;++k) {
            const auto& s=sections.c[k];
            const double out=e+z1[k];
            const double next1=s.g1*e-s.a1*z1[k]+z2[k];
            const double next2=s.g2*e-s.a2*z1[k];
            z1[k]=next1; z2[k]=next2; e=out;
        }
    }
};
}
