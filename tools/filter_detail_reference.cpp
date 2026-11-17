// filter_detail_reference.cpp — 生成「滤波器详情」静态数据（构建期）。
//
// 为什么在构建期生成：详情面板要展示真实生产系数的幅频/群延迟/冲激响应。
// 运行时按需设计 79 个预设会让 UI 卡顿（sinc-M 的原型可达百万抽头），
// 而把测量结果编进 exe 既不占启动时间，也保证与当前预设表同源。
//
// 口径（必须写清，避免把示意当实测）：
//   · 参考配置：1x 预设 44.1 kHz → 176.4 kHz（4x）；Nx 专用 96 kHz → 384 kHz（4x）。
//   · 系数来自 dsp_measure::prototype()（= review2::production_config + PolyphaseResampler），
//     与 filter_sweep / filter_report 同一条生产路径。
//   · 幅频是 65,536 点 FFT（32,769 个等间隔频点）的全频带证书；群延迟/冲激仍是
//     有限支撑/有限网格测量，UI 曲线为 512/64/129 点。
#include "dsp_measure.h"
#include "dsp/fft.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr int kResponseCount = 512;
constexpr int kPassCount = 256;
constexpr int kGdCount = 64;
constexpr int kImpulseCount = 129;
constexpr int kFullBandFft = 65536;

struct Detail {
    std::string name, window, phase, family;
    int taps = 0, T = 0, L = 1;
    bool apodizing = false, twoStage = false, forNx = false;
    double passHz = 0, stopStartHz = 0, designDb = 0, targetDb = 0;
    double passDb = 0, stopDb = 0, dcDb = 0;
    double preMs = 0, postMs = 0, gd1kMs = 0, latencyMs = 0;
    double fs = 0, inRate = 0, outRate = 0;
    int fullBandPoints = 0;
    double fullBandStopDb = 0, fullBandPassDevDb = 0, fullBandMaxStepDb = 0;
    std::vector<float> response, passCurve, gdCurve, impulse;
};

std::complex<double> response_at(const std::vector<double>& h, double hz, double fs) {
    const double w = 2.0 * kPi * hz / fs;
    std::complex<double> s{0.0, 0.0};
    for (size_t i = 0; i < h.size(); ++i)
        s += h[i] * std::complex<double>(std::cos(w * (double)i), -std::sin(w * (double)i));
    return s;
}

double group_delay_ms(const std::vector<double>& h, double hz, double fs) {
    const double df = std::max(0.5, hz * 1e-3);
    const double f1 = std::max(0.1, hz - df), f2 = hz + df;
    const auto a = response_at(h, f1, fs), b = response_at(h, f2, fs);
    if (std::abs(a) < 1e-14 || std::abs(b) < 1e-14) return std::numeric_limits<double>::quiet_NaN();
    const double dphi = std::remainder(std::arg(b) - std::arg(a), 2.0 * kPi);
    return -dphi / (2.0 * kPi * (f2 - f1)) * 1000.0;
}

std::string window_cn(rta::dsp::WindowKind w) {
    switch (w) {
        case rta::dsp::WindowKind::Kaiser: return "Kaiser";
        case rta::dsp::WindowKind::BlackmanHarris: return "Blackman-Harris";
        case rta::dsp::WindowKind::Gaussian: return "Gaussian";
        case rta::dsp::WindowKind::Hann: return "Hann";
        default: return "Window";
    }
}
std::string phase_cn(rta::dsp::PhaseKind p) {
    switch (p) {
        case rta::dsp::PhaseKind::Linear: return "线性相位";
        case rta::dsp::PhaseKind::Minimum: return "最小相位";
        case rta::dsp::PhaseKind::Intermediate: return "混合相位";
        default: return "未知相位";
    }
}

Detail analyse(const rta::dsp::FilterPreset& p) {
    Detail d;
    d.name = p.name;
    d.window = window_cn(p.window);
    d.phase = phase_cn(p.phase);
    d.forNx = p.forNx;
    d.apodizing = p.apodizing;
    const unsigned in = p.forNx ? 96000u : 44100u;
    const unsigned out = in * 4u;
    dsp_measure::Result r;
    auto h = dsp_measure::prototype(p, in, out, r, false, -1);
    if (h.empty() || !r.finite) throw std::runtime_error(std::string("prototype failed: ") + p.name);
    // dsp_measure::prototype() 已按 upFactor 归一化（filter_report/filter_sweep 同源），
    // 这里不能再除一次：再除 L 会让 DC 变成 -12 dB（L=4）而所有通带读数一起塌。
    d.taps = (int)h.size();
    d.T = r.T;
    d.L = std::max(1, r.L);
    d.twoStage = r.cascade;
    d.fs = r.fs;
    d.inRate = (double)in;
    d.outRate = (double)out;
    d.passHz = r.passHz;
    d.stopStartHz = r.stopHz;
    d.targetDb = r.target;
    d.designDb = review2::production_config(p, in, out, false).attenDb;
    d.dcDb = dsp_measure::direct(h, 0.0, d.fs);

    // -60 dB 幅度包络（与 dsp_ringing_reference 同口径）。
    std::vector<double> a(h.size());
    double peak = 0.0;
    for (size_t i = 0; i < h.size(); ++i) { a[i] = std::abs(h[i]); peak = std::max(peak, a[i]); }
    if (!(peak > 0)) throw std::runtime_error(std::string("zero prototype: ") + p.name);
    const double threshold = peak * 0.001;
    const size_t first = (size_t)(std::find_if(a.begin(), a.end(), [&](double v){ return v >= threshold; }) - a.begin());
    const size_t last = a.size() - 1 - (size_t)(std::find_if(a.rbegin(), a.rend(), [&](double v){ return v >= threshold; }) - a.rbegin());
    const size_t peakFirst = (size_t)(std::find(a.begin(), a.end(), peak) - a.begin());
    const size_t peakLast = a.size() - 1 - (size_t)(std::find(a.rbegin(), a.rend(), peak) - a.rbegin());
    const double center = 0.5 * (double)(peakFirst + peakLast);
    d.preMs = std::max(0.0, center - (double)first) / d.fs * 1000.0;
    d.postMs = std::max(0.0, (double)last - center) / d.fs * 1000.0;

    // 全频带 FFT：把生产原型零填充到 65536 点，得到 32769 个等间隔频点。
    // 这不是"有限网格近似"——它是该 FIR 的精确 DTFT 采样；65,536 点 >> 抽头数，
    // 阻带里不可能夹着比 bin 间隔更窄的尖峰而漏测。
    int N = kFullBandFft;
    while (N < (int)h.size() * 2) N <<= 1;
    std::vector<rta::dsp::cplx> spec((size_t)N);
    for (size_t i = 0; i < h.size(); ++i) spec[i] = rta::dsp::cplx(h[i], 0.0);
    rta::dsp::fft(spec, false);
    std::vector<double> magDb((size_t)N / 2 + 1);
    for (int k = 0; k <= N / 2; ++k) {
        const double a = std::abs(spec[(size_t)k]);
        magDb[(size_t)k] = 20.0 * std::log10(std::max(a, 1e-15));
    }
    auto mag_at = [&](double hz) {
        if (!(hz > 0.0)) return magDb[0];
        const double pos = hz / d.fs * (double)N;
        if (pos >= (double)(N / 2)) return magDb[(size_t)(N / 2)];
        const size_t k = (size_t)pos;
        const double t = pos - (double)k;
        return magDb[k] * (1.0 - t) + magDb[k + 1] * t;
    };
    d.fullBandPoints = N / 2 + 1;
    d.dcDb = magDb[0];
    // 幅频：10 Hz..原型 Nyquist，对数网格（512 点，从全频带 FFT 插值）。
    d.response.resize(kResponseCount);
    const double top = d.fs * 0.5;
    for (int i = 0; i < kResponseCount; ++i) {
        const double t = (double)i / (double)(kResponseCount - 1);
        const double hz = 10.0 * std::pow(top / 10.0, t);
        d.response[(size_t)i] = (float)std::max(mag_at(hz), -400.0);
    }
    // 通带放大：0..passHz 线性网格（256 点）。
    d.passCurve.resize(kPassCount);
    d.passDb = 0.0;
    for (int i = 0; i < kPassCount; ++i) {
        const double hz = d.passHz * (double)i / (double)(kPassCount - 1);
        const double v = mag_at(hz);
        d.passCurve[(size_t)i] = (float)v;
        if (std::abs(v) > std::abs(d.passDb)) d.passDb = v;
    }
    // 阻带：直接在 32769 个 FFT 频点上取最大值（不是 512 点插值）。
    d.stopDb = -400.0;
    for (int k = 0; k <= N / 2; ++k) {
        const double hz = (double)k * d.fs / (double)N;
        if (hz >= d.stopStartHz && hz <= top) d.stopDb = std::max(d.stopDb, magDb[(size_t)k]);
    }
    d.fullBandStopDb = d.stopDb;
    d.fullBandPassDevDb = d.passDb;
    d.fullBandMaxStepDb = 0.0;
    const int startBin = std::max(1, (int)std::ceil(10.0 / d.fs * (double)N));
    for (int k = startBin; k < N / 2; ++k)
        d.fullBandMaxStepDb = std::max(d.fullBandMaxStepDb, std::abs(magDb[(size_t)k + 1] - magDb[(size_t)k]));
    // 群延迟：100 Hz..min(20k, passHz)，对数网格。
    d.gdCurve.resize(kGdCount);
    const double gdTop = std::min(20000.0, d.passHz);
    for (int i = 0; i < kGdCount; ++i) {
        const double t = (double)i / (double)(kGdCount - 1);
        const double hz = 100.0 * std::pow(gdTop / 100.0, t);
        const double v = group_delay_ms(h, hz, d.fs);
        d.gdCurve[(size_t)i] = (float)(std::isfinite(v) ? std::clamp(v, -10.0, 1000.0) : 0.0);
    }
    d.gd1kMs = group_delay_ms(h, 1000.0, d.fs);
    d.latencyMs = group_delay_ms(h, 100.0, d.fs);
    // 冲激：峰值两侧各 32 点，归一化。
    d.impulse.resize(kImpulseCount);
    const size_t pk = (size_t)std::llround(center);
    for (int i = 0; i < kImpulseCount; ++i) {
        const long idx = (long)pk + (i - kImpulseCount / 2);
        d.impulse[(size_t)i] = (idx >= 0 && (size_t)idx < h.size()) ? (float)(h[(size_t)idx] / peak) : 0.0f;
    }
    d.family = d.apodizing ? "切趾" : (p.window == rta::dsp::WindowKind::Gaussian ? "高斯" : "标准");
    return d;
}

std::string emit_header(const std::vector<Detail>& rows) {
    std::ostringstream o;
    o << "#pragma once\n// Generated by filter_detail_reference. Do not edit.\n"
      << "// Reference: 1x 44.1k->176.4k, Nx 96k->384k; production coefficients.\n"
      << "namespace rta::ui_reference {\n"
      << "struct FilterDetail {\n"
      << "  const char* name; const char* window; const char* phase; const char* family;\n"
      << "  int taps; int T; int L; bool apodizing; bool twoStage; bool forNx;\n"
      << "  float passHz; float stopStartHz; float designDb; float targetDb;\n"
      << "  float passDb; float stopDb; float dcDb; float preMs; float postMs;\n"
      << "  float gd1kMs; float latencyMs; float fs; float inRate; float outRate;\n"
      << "  int fullBandPoints; float fullBandStopDb; float fullBandPassDevDb; float fullBandMaxStepDb;\n"
      << "  const float* response; int responseCount;\n"
      << "  const float* passCurve; int passCount;\n"
      << "  const float* gdCurve; int gdCount;\n"
      << "  const float* impulse; int impulseCount;\n"
      << "};\n\n";
    auto arr = [&](const char* name, const std::vector<float>& v) {
        o << "inline const float " << name << "[] = {";
        o << std::setprecision(9);
        for (size_t i = 0; i < v.size(); ++i) { if (i) o << ','; o << v[i]; }
        o << "};\n";
    };
    for (size_t i = 0; i < rows.size(); ++i) {
        arr(("kFdResp" + std::to_string(i)).c_str(), rows[i].response);
        arr(("kFdPass" + std::to_string(i)).c_str(), rows[i].passCurve);
        arr(("kFdGd" + std::to_string(i)).c_str(), rows[i].gdCurve);
        arr(("kFdImp" + std::to_string(i)).c_str(), rows[i].impulse);
    }
    o << "\ninline const FilterDetail kFilterDetails[] = {\n";
    o << std::setprecision(9);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& d = rows[i];
        o << "  {\"" << d.name << "\",\"" << d.window << "\",\"" << d.phase << "\",\"" << d.family << "\","
          << d.taps << ',' << d.T << ',' << d.L << ',' << (d.apodizing?"true":"false") << ','
          << (d.twoStage?"true":"false") << ',' << (d.forNx?"true":"false") << ','
          << d.passHz << ',' << d.stopStartHz << ',' << d.designDb << ',' << d.targetDb << ','
          << d.passDb << ',' << d.stopDb << ',' << d.dcDb << ',' << d.preMs << ',' << d.postMs << ','
          << d.gd1kMs << ',' << d.latencyMs << ',' << d.fs << ',' << d.inRate << ',' << d.outRate << ','
          << d.fullBandPoints << ',' << d.fullBandStopDb << ',' << d.fullBandPassDevDb << ',' << d.fullBandMaxStepDb << ','
          << "kFdResp" << i << ',' << d.response.size() << ','
          << "kFdPass" << i << ',' << d.passCurve.size() << ','
          << "kFdGd" << i << ',' << d.gdCurve.size() << ','
          << "kFdImp" << i << ',' << d.impulse.size() << "},\n";
    }
    o << "};\ninline const int kFilterDetailCount = (int)(sizeof(kFilterDetails)/sizeof(kFilterDetails[0]));\n"
      << "} // namespace rta::ui_reference\n";
    return o.str();
}
} // namespace

int main(int argc, char** argv) {
    std::string header = "filter_detail_reference.h";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--header" && i + 1 < argc) header = argv[++i];
    }
    try {
        std::vector<Detail> rows;
        for (const auto& p : rta::dsp::filter_presets()) rows.push_back(analyse(p));
        const auto text = emit_header(rows);
        std::ofstream f(header, std::ios::binary);
        f << text;
        f.flush();
        if (!f) throw std::runtime_error("cannot write " + header);
        std::cerr << "filter details: " << rows.size() << " presets -> " << header << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
