// eq_text.h — REW / AutoEq 参数均衡器文本解析
//
// 支持两种常见格式（手册 7 章 Matrix 的 "Process" 可直接吃这类文件）：
//
//   REW 导出：
//     Filter  1: ON PK     Fc   100.0 Hz  Gain   3.0 dB  Q  1.400
//     Filter  2: ON LS     Fc    80.0 Hz  Gain   2.0 dB  Q  0.700
//
//   AutoEq（多一行 Preamp）：
//     Preamp: -6.0 dB
//     Filter 1: ON PK Fc 105 Hz Gain 5.5 dB Q 0.70
//
// 类型：PK 峰值 / LS 低架 / HS 高架 / LP 低通 / HP 高通 / BP 带通 / NO 陷波 / AP 全通
#pragma once
#include "matrix.h"
#include <string>
#include <vector>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <algorithm>

namespace rta::dsp {

struct EqBand {
    bool        enabled = true;
    std::string type;        // "PK" / "LS" / ...
    double      fc   = 1000.0;
    double      gain = 0.0;  // dB（PK / LS / HS 有效）
    double      q    = 1.0;
    bool        hasQ = true;
};

struct EqSpec {
    double               preampDb = 0.0;
    std::vector<EqBand>  bands;
};

namespace detail {
inline std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
// 在字符串里找 keyword（不区分大小写），返回其后的第一个数值
inline bool value_after(const std::string& line, const char* key, double& out) {
    const std::string L = upper(line), K = upper(key);
    const size_t p = L.find(K);
    if (p == std::string::npos) return false;
    size_t i = p + K.size();
    // 跳过 = 与空白
    while (i < line.size() && (line[i] == '=' || line[i] == ':' || std::isspace((unsigned char)line[i]))) ++i;
    char* end = nullptr;
    const double v = std::strtod(line.c_str() + i, &end);
    if (end == line.c_str() + i) return false;      // 没有数字
    out = v;
    return true;
}
}

// 解析一行；返回 false 表示这行不是有效的滤波器定义
inline bool parse_eq_line(const std::string& line, EqBand& band) {
    if (line.find("Filter") == std::string::npos) return false;

    // ON / OFF
    const std::string L = detail::upper(line);
    band.enabled = (L.find("OFF") == std::string::npos);

    // 类型：在 "Filter N:" 之后找第一个已知的 2 字母类型
    static const char* types[] = {"PK","LS","HS","LP","HP","BP","NO","AP","LSC","HSC"};
    band.type.clear();
    const size_t colon = line.find(':');
    const size_t from = (colon == std::string::npos) ? 0 : colon + 1;
    for (const char* t : types) {
        const size_t p = L.find(t, from);
        if (p != std::string::npos) { band.type = t; break; }
    }
    if (band.type.empty()) return false;

    double v = 0.0;
    if (detail::value_after(line, "Fc", v))   band.fc = v;
    else if (detail::value_after(line, "F ", v)) band.fc = v;
    band.gain = detail::value_after(line, "Gain", v) ? v : 0.0;
    band.hasQ = detail::value_after(line, "Q", v);
    if (band.hasQ) band.q = v;
    else band.q = 0.707;
    return true;
}

// 解析整份文本
inline EqSpec parse_eq_text(const std::string& text) {
    EqSpec spec;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        pos = nl + 1;
        if (line.empty()) continue;

        double v = 0.0;
        if (detail::upper(line).find("PREAMP") != std::string::npos) {
            if (detail::value_after(line, "Preamp", v)) { spec.preampDb = v; continue; }
        }
        EqBand b;
        if (parse_eq_line(line, b)) spec.bands.push_back(b);
    }
    return spec;
}

// 把一段参数 EQ 变成双二阶级联
inline std::vector<Biquad> build_eq_filters(const EqSpec& spec, double fs) {
    std::vector<Biquad> out;
    out.reserve(spec.bands.size());
    for (const auto& b : spec.bands) {
        if (!b.enabled) continue;
        if (b.fc <= 0.0) continue;
        // Nyquist 保护：上限钳位到 0.49 * fs，避免 Biquad 系数在 Nyquist 发散
        const double fc = (fs > 0.0 && b.fc >= 0.49 * fs) ? 0.49 * fs : b.fc;
        Biquad q;
        const std::string t = detail::upper(b.type);
        if      (t == "PK")  biquad_design::peak(q, fs, fc, b.q, b.gain);
        else if (t == "LS" || t == "LSC") {
            if (b.hasQ) biquad_design::lowShelfQ(q, fs, fc, b.q, b.gain);
            else        biquad_design::lowShelf(q, fs, fc, 0.7, b.gain);
        }
        else if (t == "HS" || t == "HSC") {
            if (b.hasQ) biquad_design::highShelfQ(q, fs, fc, b.q, b.gain);
            else        biquad_design::highShelf(q, fs, fc, 0.7, b.gain);
        }
        else if (t == "LP")  biquad_design::lowpass(q, fs, fc, b.q);
        else if (t == "HP")  biquad_design::highpass(q, fs, fc, b.q);
        else if (t == "BP")  biquad_design::bandpass(q, fs, fc, b.q);
        else if (t == "NO")  biquad_design::notch(q, fs, fc, b.q);
        else if (t == "AP")  biquad_design::allpass(q, fs, fc, b.q);
        else continue;
        out.push_back(q);
    }
    return out;
}

// 级联幅频响应（dB）
inline double eq_chain_response_db(const std::vector<Biquad>& chain, double freqNorm) {
    double sum = 0.0;
    for (const auto& q : chain) sum += q.response_db(freqNorm);
    return sum;
}

} // namespace rta::dsp
