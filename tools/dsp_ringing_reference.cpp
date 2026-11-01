#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include "dsp_measure.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
using namespace rta::dsp;
struct Row {
  std::string name, phase, hash;
  unsigned in = 0, out = 0;
  size_t taps = 0, peakFirst = 0, peakLast = 0, firstAbove = 0, lastAbove = 0;
  double fs = 0, peak = 0, threshold = 0, preCross = 0, postCross = 0,
         preMs = 0, postMs = 0, gdMs = 0;
};
static const char *definition =
    "Discrete absolute-coefficient peak envelope: before the global peak "
    "Epre[n]=max(abs(h[0..n])); "
    "after the peak Epost[n]=max(abs(h[n..N-1])). threshold=max(abs(h))*0.001 "
    "(-60 dB amplitude). "
    "Boundaries are first/last actual samples at or above threshold, with "
    "one-output-sample boundary resolution. "
    "Peak time is the midpoint of first/last exactly equal global maxima. "
    "Group delay is a separate analytic derivative at 1000 Hz; MP/IP group "
    "delay varies with frequency. "
    "Finite FIR coefficient support only, not continuous acoustic ringing or a "
    "listening verdict.";
std::string sha256(const std::vector<double> &h) {
  BCRYPT_ALG_HANDLE a = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD length = 0, got = 0;
  auto ok = [](NTSTATUS s) {
    if (s < 0)
      throw std::runtime_error("Windows SHA256 failed");
  };
  ok(BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
  ok(BCryptGetProperty(a, BCRYPT_OBJECT_LENGTH, (PUCHAR)&length, sizeof(length),
                       &got, 0));
  std::vector<unsigned char> obj(length), digest(32);
  ok(BCryptCreateHash(a, &hash, obj.data(), length, nullptr, 0, 0));
  ok(BCryptHashData(hash, (PUCHAR)h.data(), (ULONG)(h.size() * sizeof(double)),
                    0));
  ok(BCryptFinishHash(hash, digest.data(), 32, 0));
  BCryptDestroyHash(hash);
  BCryptCloseAlgorithmProvider(a, 0);
  std::ostringstream s;
  s << std::hex << std::setfill('0');
  for (auto c : digest)
    s << std::setw(2) << unsigned(c);
  return s.str();
}
std::string quote(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '\\' || c == '\"')
      o += '\\';
    o += c;
  }
  return o + '\"';
}
Row analyse(const std::vector<double> &h, const FilterPreset &p, unsigned in,
            unsigned out, double fs) {
  Row r;
  r.name = p.name;
  r.phase = phase_name(p.phase);
  r.in = in;
  r.out = out;
  r.fs = fs;
  r.taps = h.size();
  if (h.empty())
    throw std::runtime_error("empty prototype");
  std::vector<double> a(h.size());
  for (size_t i = 0; i < h.size(); ++i) {
    if (!std::isfinite(h[i]))
      throw std::runtime_error("nonfinite prototype");
    a[i] = abs(h[i]);
    r.peak = std::max(r.peak, a[i]);
  }
  if (!(r.peak > 0))
    throw std::runtime_error("zero prototype");
  r.hash = sha256(h);
  r.threshold = r.peak * .001;
  r.peakFirst = std::find(a.begin(), a.end(), r.peak) - a.begin();
  r.peakLast =
      a.size() - 1 - (std::find(a.rbegin(), a.rend(), r.peak) - a.rbegin());
  double center = .5 * (r.peakFirst + r.peakLast);
  r.firstAbove = std::find_if(a.begin(), a.end(),
                              [&](double v) { return v >= r.threshold; }) -
                 a.begin();
  r.lastAbove = a.size() - 1 -
                (std::find_if(a.rbegin(), a.rend(),
                              [&](double v) { return v >= r.threshold; }) -
                 a.rbegin());
  r.preCross = (double)r.firstAbove;
  r.postCross = (double)r.lastAbove;
  r.preMs = std::max(0., center - r.preCross) / fs * 1000;
  r.postMs = std::max(0., r.postCross - center) / fs * 1000;
  std::complex<double> s{}, n{};
  for (size_t i = 0; i < h.size(); ++i) {
    double angle = -2 * kPi * 1000. / fs * i;
    auto z = h[i] * std::complex<double>(std::cos(angle), std::sin(angle));
    s += z;
    n += z * double(i);
  }
  r.gdMs = (n / s).real() / fs * 1000;
  return r;
}
std::vector<std::string> split(const std::string &s) {
  std::vector<std::string> o;
  std::istringstream a(s);
  std::string x;
  while (std::getline(a, x, '\t'))
    o.push_back(x);
  return o;
}
bool selected(unsigned in, unsigned out) {
  return (in == 44100 && out == 1411200) || (in == 48000 && out == 1536000) ||
         (in == 96000 && out == 384000);
}
int selftest() {
  FilterPreset p{};
  p.name = "fixture";
  p.phase = PhaseKind::Minimum;
  int failures = 0;
  auto check = [&](const std::vector<double> &h, double pre, double post) {
    auto r = analyse(h, p, 1000, 2000, 1000);
    bool ok = r.preMs == pre && r.postMs == post;
    failures += !ok;
    std::cout << "ENVELOPE " << pre << "/" << post << " got=" << r.preMs << "/"
              << r.postMs << " " << (ok ? "PASS" : "FAIL") << "\n";
  };
  check({0, 0, 1, 0, 0}, 0, 0);
  check({0, .0005, 0, .002, 0, 1, 0, .002, 0, .0005, 0}, 2, 2);
  check({0, .0005, .002, 1, 1, .002, .0005, 0}, 1.5, 1.5);
  check({0, -.002, 1, -.002, 0}, 1, 1);
  check({.01, .0005, 0, .002, 0, 1, 0, .002, 0, .0005, 0}, 5, 2);
  std::cout << "ENVELOPE_SELFTEST failures=" << failures << "\n";
  return failures ? 1 : 0;
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--selftest")
      return selftest();
    std::string table, coeff, header, json;
    bool production = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto value = [&]() {
        if (++i == argc)
          throw std::runtime_error("missing option value");
        return std::string(argv[i]);
      };
      if (a == "--production")
        production = true;
      else if (a == "--table")
        table = value();
      else if (a == "--coefficients")
        coeff = value();
      else if (a == "--header")
        header = value();
      else if (a == "--json")
        json = value();
      else
        throw std::runtime_error("unknown option " + a);
    }
    if (header.empty() || json.empty() ||
        (!production && (table.empty() || coeff.empty())) ||
        (production && (!table.empty() || !coeff.empty())))
      throw std::runtime_error(
          "Usage: dsp_ringing_reference --production --header FILE --json FILE "
          "OR --table TSV --coefficients DIR --header FILE --json FILE");
    std::vector<Row> rows;
    if (production) {
      for (auto g : {std::pair{44100u, 1411200u}, std::pair{48000u, 1536000u},
                     std::pair{96000u, 384000u}})
        for (auto &p : filter_presets()) {
          dsp_measure::Result meta;
          auto h = dsp_measure::prototype(p, g.first, g.second, meta);
          rows.push_back(analyse(h, p, g.first, g.second, meta.fs));
        }
    } else {
      std::ifstream t(table);
      if (!t)
        throw std::runtime_error("cannot read table");
      std::string line;
      std::vector<std::string> keys;
      while (std::getline(t, line)) {
        if (line.empty() || line[0] == '#')
          continue;
        auto cells = split(line);
        if (keys.empty()) {
          keys = cells;
          continue;
        }
        std::map<std::string, std::string> m;
        for (size_t k = 0; k < keys.size() && k < cells.size(); ++k)
          m[keys[k]] = cells[k];
        unsigned in = std::stoul(m.at("in_rate")),
                 out = std::stoul(m.at("out_rate"));
        if (!selected(in, out))
          continue;
        auto *p = find_preset(m.at("preset"));
        if (!p)
          throw std::runtime_error("unknown preset");
        auto path = std::filesystem::u8path(coeff) /
                    (m.count("coefficient_file")
                         ? m.at("coefficient_file")
                         : std::to_string(in) + "-" + std::to_string(out) +
                               "-" + p->name + ".f64");
        size_t taps = std::stoull(m.at("taps"));
        if (std::filesystem::file_size(path) != 8 * taps)
          throw std::runtime_error("coefficient length mismatch");
        std::vector<double> h(taps);
        std::ifstream b(path, std::ios::binary);
        b.read((char *)h.data(), 8 * taps);
        if (!b)
          throw std::runtime_error("coefficient read failed");
        rows.push_back(
            analyse(h, *p, in, out, std::stod(m.at("prototype_rate"))));
      }
    }
    if (rows.size() != 237)
      throw std::runtime_error(
          "expected exactly79 presets times3 production rate pairs");
    for (auto path : {header, json}) {
      auto p = std::filesystem::u8path(path);
      if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());
    }
    std::ofstream h(header), j(json);
    h << std::setprecision(17);
    j << std::setprecision(17);
    h << "// Generated by dsp_ringing_reference; never edit by hand.\n// "
      << definition
      << "\n#pragma once\n#include <array>\n#include <cstdint>\nnamespace "
         "rta::ui_reference {\nstruct RingingReference {const char*name; "
         "uint32_t inRate,outRate; double "
         "pre60Ms,post60Ms,groupDelay1kMs;const char*sha256; uint64_t "
         "taps,peakFirst,peakLast;};\ninline constexpr "
         "std::array<RingingReference,237> kRingingReference{{\n";
    j << "{\n\"schema\":1,\n\"threshold_db\":-60,\n\"group_delay_hz\":1000,"
         "\n\"definition\":"
      << quote(definition) << ",\n\"rows\":[\n";
    for (size_t i = 0; i < rows.size(); ++i) {
      auto &r = rows[i];
      h << "{" << quote(r.name) << "," << r.in << "," << r.out << "," << r.preMs
        << "," << r.postMs << "," << r.gdMs << "," << quote(r.hash) << ","
        << r.taps << "," << r.peakFirst << "," << r.peakLast << "},\n";
      j << "{\"name\":" << quote(r.name) << ",\"phase\":" << quote(r.phase)
        << ",\"in_rate\":" << r.in << ",\"out_rate\":" << r.out
        << ",\"prototype_rate\":" << r.fs << ",\"taps\":" << r.taps
        << ",\"sha256\":" << quote(r.hash)
        << ",\"peak_first_index\":" << r.peakFirst
        << ",\"peak_last_index\":" << r.peakLast
        << ",\"peak_absolute\":" << r.peak
        << ",\"threshold_absolute\":" << r.threshold
        << ",\"pre_cross_index\":" << r.preCross
        << ",\"post_cross_index\":" << r.postCross
        << ",\"first_sample_above_threshold\":" << r.firstAbove
        << ",\"last_sample_above_threshold\":" << r.lastAbove
        << ",\"pre60_ms\":" << r.preMs << ",\"post60_ms\":" << r.postMs
        << ",\"group_delay_1khz_ms\":" << r.gdMs << "}"
        << (i + 1 == rows.size() ? "\n" : ",\n");
    }
    h << "}};\n} // namespace rta::ui_reference\n";
    j << "]\n}\n";
    if (!h || !j)
      throw std::runtime_error("output write failed");
    std::cout << "RINGING rows=" << rows.size()
              << " mode=" << (production ? "production" : "exported")
              << " threshold=-60dB group_delay=1000Hz\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
