// peq.h — 20 段参数均衡（PEQ, Parametric EQ），HIFI 音质导向（261024-02 #6）
// -----------------------------------------------------------------------------
// 设计要点（详见 docs/WIP-t25-eq.md）：
//   * RBJ Audio EQ Cookbook 双二阶：peaking / low-shelf / high-shelf；
//     系数用 float64 计算，采样处理用 float32（z1/z2 状态为 float）。
//   * 默认 20 段，中心频率取 ISO 1/3 倍频程序列（20Hz~20kHz）中的 20 个；
//     每段默认 Q=1.41、增益 0dB（±12dB 步进 0.1 由 UI 约束）。
//   * 采样率相关：prepare(fs) 按当前采样率重建全部系数（44.1/48/96/192k …）。
//   * 热更新：set_settings() 不打断音频线程；新系数链从零状态启动，
//     与旧链做 N 样本线性交叉淡入（默认 128 样本），避免爆音。
//   * bypass 作为「空系数链」参与交叉淡入，开关时不产生阶跃。
//   * 纯头文件、无第三方依赖；算法参考 RBJ Audio EQ Cookbook（见 WIP 文档链接），
//     未复制任何 GPL 实现代码。
#pragma once

#include "matrix.h"          // 复用 kPi；Biquad 设计公式与本项目既有一致
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace rta::dsp {

// ---------------- 数据模型 ----------------
enum class PeqBandType { Peak, LowShelf, HighShelf };

inline const char* peq_band_type_name(PeqBandType t) {
    switch (t) {
        case PeqBandType::LowShelf:  return "LS";
        case PeqBandType::HighShelf: return "HS";
        default:                     return "PK";
    }
}
inline bool peq_band_type_from_name(const std::string& s, PeqBandType& out) {
    if (s == "PK" || s == "pk" || s == "PEAK" || s == "peak") { out = PeqBandType::Peak; return true; }
    if (s == "LS" || s == "ls" || s == "LOW_SHELF" || s == "lowshelf") { out = PeqBandType::LowShelf; return true; }
    if (s == "HS" || s == "hs" || s == "HIGH_SHELF" || s == "highshelf") { out = PeqBandType::HighShelf; return true; }
    return false;
}

struct PeqBand {
    bool         enabled = true;
    PeqBandType  type    = PeqBandType::Peak;
    double       freq    = 1000.0;   // Hz
    double       gainDb  = 0.0;      // dB
    double       q       = 1.41;     // 品质因数（架式滤波器也用它，Q 形式 RBJ 公式）
};

struct PeqSettings {
    bool                 bypass    = false;
    double               preampDb  = 0.0;    // 前级增益（dB），用于抵消提升造成的削波
    std::string          presetName = "Flat";
    std::vector<PeqBand> bands;
};

// 默认 20 段中心频率：取自 ISO 1/3 倍频程中心频率序列（20Hz~20kHz）。
// 完整序列是 31 段，这里选 20 段覆盖全频段，并**保留 1kHz 基准点**：
//   20,25,40,63,80,125,200,315,500,800,1000,1250,2000,3150,5000,8000,10000,12500,16000,20000
// UI / 文档 / 测试均以本函数为唯一真相源。
inline std::vector<double> peq_default_frequencies() {
    return { 20.0, 25.0, 40.0, 63.0, 80.0, 125.0, 200.0, 315.0, 500.0, 800.0,
             1000.0, 1250.0, 2000.0, 3150.0, 5000.0, 8000.0, 10000.0, 12500.0,
             16000.0, 20000.0 };
}

inline PeqSettings peq_flat_settings() {
    PeqSettings s;
    s.presetName = "Flat";
    const auto fs = peq_default_frequencies();
    s.bands.reserve(fs.size());
    for (double f : fs) {
        PeqBand b;
        b.enabled = true;
        b.type = PeqBandType::Peak;
        b.freq = f;
        b.gainDb = 0.0;
        b.q = 1.41;
        s.bands.push_back(b);
    }
    return s;
}

// ---------------- 内置预设（示例，不是测量目标曲线） ----------------
inline std::vector<std::string> peq_builtin_preset_names() {
    return { "Flat", "Harman 风格（示例）", "低频增强", "人声清晰" };
}

inline bool peq_builtin_preset(const std::string& name, PeqSettings& out) {
    out = peq_flat_settings();
    out.presetName = name;
    if (name == "Flat" || name.empty()) return true;
    const auto f = peq_default_frequencies();
    auto set = [&](size_t i, double gain) {
        if (i < out.bands.size()) out.bands[i].gainDb = gain;
    };
    if (name == "Harman 风格（示例）") {
        // 近似 Harman 头戴目标（低频温和提升 + 3k 附近耳增益），仅作示例，不是测量数据
        const double g[20] = { 5.0, 5.0, 4.0, 3.0, 2.5, 1.0, 0.0, 0.0, 0.0, 0.0,
                               0.5, 1.0, 2.0, 1.0, 0.0, -0.5, -1.0, -1.0, -0.5, 0.0 };
        for (size_t i = 0; i < out.bands.size() && i < 20; ++i) set(i, g[i]);
        out.preampDb = -5.0;
        return true;
    }
    if (name == "低频增强") {
        out.bands[0].type = PeqBandType::LowShelf;  out.bands[0].freq = 80.0;  out.bands[0].gainDb = 6.0; out.bands[0].q = 0.7;
        set(2, 3.0); set(3, 2.0); set(4, 1.0);
        out.preampDb = -6.0;
        return true;
    }
    if (name == "人声清晰") {
        set(9, 1.0); set(10, 2.0); set(11, 3.0); set(12, 3.0); set(13, 2.0); set(14, 1.0);
        out.preampDb = -3.0;
        return true;
    }
    (void)f;
    return false;
}

// ---------------- 文本序列化（保存/加载/预设文件共用） ----------------
// 格式（v1，一行一项）：
//   bypass=0
//   preamp=-3.0
//   name=Flat
//   band=1,PK,20,0.0,1.41        # 序号(仅注释用),类型,频率,增益,Q
inline std::string peq_to_text(const PeqSettings& s) {
    std::ostringstream o;
    o << "# rlplayer PEQ v1\n";
    o << "bypass=" << (s.bypass ? 1 : 0) << "\n";
    o << "preamp=" << s.preampDb << "\n";
    o << "name=" << (s.presetName.empty() ? "Custom" : s.presetName) << "\n";
    for (size_t i = 0; i < s.bands.size(); ++i) {
        const auto& b = s.bands[i];
        o << "band=" << (i + 1) << "," << peq_band_type_name(b.type) << ","
          << b.freq << "," << b.gainDb << "," << b.q
          << (b.enabled ? "" : ",off") << "\n";
    }
    return o.str();
}

inline bool peq_from_text(const std::string& text, PeqSettings& out, std::string* err = nullptr) {
    PeqSettings s;
    bool anyBand = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "bypass") {
            s.bypass = (val == "1" || val == "true" || val == "on");
        } else if (key == "preamp") {
            s.preampDb = std::atof(val.c_str());
        } else if (key == "name") {
            s.presetName = val;
        } else if (key == "band") {
            PeqBand b;
            char type[8] = {0};
            double f = 1000.0, g = 0.0, q = 1.41;
            char tail[16] = {0};
            const int n = std::sscanf(val.c_str(), "%*d,%7[^,],%lf,%lf,%lf,%15s", type, &f, &g, &q, tail);
            if (n < 4) continue;
            PeqBandType t;
            if (!peq_band_type_from_name(type, t)) t = PeqBandType::Peak;
            b.type = t; b.freq = f; b.gainDb = g; b.q = q;
            b.enabled = !(std::strstr(tail, "off") || std::strstr(tail, "0"));
            s.bands.push_back(b);
            anyBand = true;
        }
    }
    if (!anyBand) {
        if (err) *err = "没有 band= 行";
        return false;
    }
    out = s;
    return true;
}

// ---------------- 系数设计（float64） ----------------
struct PeqCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;   // a0 已归一化为 1

    // 解析幅频响应（dB）；freqNorm: 1.0 = Nyquist
    double response_db(double freqNorm) const {
        const double w = kPi * freqNorm;
        const std::complex<double> z1 = std::polar(1.0, -w);
        const std::complex<double> z2 = z1 * z1;
        const std::complex<double> num = b0 + b1 * z1 + b2 * z2;
        const std::complex<double> den = 1.0 + a1 * z1 + a2 * z2;
        return 20.0 * std::log10(std::abs(num) / (std::abs(den) + 1e-300) + 1e-300);
    }
};

namespace peq_detail {
inline void set_norm(PeqCoeffs& c, double b0, double b1, double b2, double a0, double a1, double a2) {
    const double inv = (std::fabs(a0) > 1e-300) ? 1.0 / a0 : 0.0;
    c.b0 = b0 * inv; c.b1 = b1 * inv; c.b2 = b2 * inv;
    c.a1 = a1 * inv; c.a2 = a2 * inv;
}
} // namespace peq_detail

// 按 RBJ Audio EQ Cookbook 设计单个双二阶（系数 double）
inline PeqCoeffs peq_design_band(const PeqBand& band, double fs) {
    PeqCoeffs c;
    if (!band.enabled || fs <= 0.0) return c;
    const double f0 = std::min(std::max(band.freq, 10.0), 0.49 * fs);
    const double q  = std::min(std::max(band.q, 0.1), 20.0);
    const double g  = std::min(std::max(band.gainDb, -24.0), 24.0);
    if (std::fabs(g) < 1e-9) return c;          // 0dB 即恒等（b0=1，其余 0）

    const double A  = std::pow(10.0, g / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);

    if (band.type == PeqBandType::Peak) {
        const double alpha = sw / (2.0 * q);
        peq_detail::set_norm(c,
            1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A,
            1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A);
        return c;
    }
    // 架式：RBJ cookbook 的 Q 形式（alpha = sin(w0)/(2Q)）
    const double alpha = sw / (2.0 * q);
    const double tsa = 2.0 * std::sqrt(A) * alpha;
    if (band.type == PeqBandType::LowShelf) {
        peq_detail::set_norm(c,
            A * ((A + 1) - (A - 1) * cw + tsa),
            2 * A * ((A - 1) - (A + 1) * cw),
            A * ((A + 1) - (A - 1) * cw - tsa),
            (A + 1) + (A - 1) * cw + tsa,
            -2 * ((A - 1) + (A + 1) * cw),
            (A + 1) + (A - 1) * cw - tsa);
    } else {
        peq_detail::set_norm(c,
            A * ((A + 1) + (A - 1) * cw + tsa),
            -2 * A * ((A - 1) + (A + 1) * cw),
            A * ((A + 1) + (A - 1) * cw - tsa),
            (A + 1) - (A - 1) * cw + tsa,
            2 * ((A - 1) - (A + 1) * cw),
            (A + 1) - (A - 1) * cw - tsa);
    }
    return c;
}

inline std::vector<PeqCoeffs> peq_design(const PeqSettings& s, double fs) {
    std::vector<PeqCoeffs> out;
    if (s.bypass) return out;
    out.reserve(s.bands.size());
    for (const auto& b : s.bands) {
        if (!b.enabled || std::fabs(b.gainDb) < 1e-9) continue;
        out.push_back(peq_design_band(b, fs));
    }
    return out;
}

inline double peq_gain_linear(const PeqSettings& s) {
    if (s.bypass) return 1.0;
    const double p = std::min(std::max(s.preampDb, -24.0), 24.0);
    return std::pow(10.0, p / 20.0);
}

// 总幅频响应（dB，含前级增益）：给定设置 + 采样率 + 频率（Hz）
inline double peq_response_db(const PeqSettings& s, double fs, double freq) {
    if (s.bypass || fs <= 0.0) return 0.0;
    const double fn = std::min(std::max(freq / (fs * 0.5), 0.0), 1.0);
    double db = 20.0 * std::log10(std::max(peq_gain_linear(s), 1e-300));
    for (const auto& c : peq_design(s, fs)) db += c.response_db(fn);
    return db;
}

// ---------------- float32 处理单元 ----------------
// 系数 double、状态 float：满足「float64 系数计算 / float32 处理」。
struct PeqBiquadF32 {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    float  z1 = 0.0f, z2 = 0.0f;

    void set(const PeqCoeffs& c) { b0 = c.b0; b1 = c.b1; b2 = c.b2; a1 = c.a1; a2 = c.a2; reset(); }
    void reset() { z1 = z2 = 0.0f; }
    inline float process(float x) {
        // 直接型转置 II（数值稳定性好）
        const float y = (float)(b0 * (double)x + (double)z1);
        z1 = (float)(b1 * (double)x - a1 * (double)y + (double)z2);
        z2 = (float)(b2 * (double)x - a2 * (double)y);
        return y;
    }
};

// ---------------- 20 段 PEQ 引擎（线程安全 + 热更新交叉淡入） ----------------
class PeqEngine {
public:
    PeqEngine() = default;
    PeqEngine(const PeqEngine&) = delete;
    PeqEngine& operator=(const PeqEngine&) = delete;

    // 播放开始 / 切歌切率：按新采样率重建系数并清状态（新曲起点，不需要交叉淡入）
    void prepare(double fs, int channels) {
        std::lock_guard<std::mutex> lk(mu_);
        const bool rateChanged = (std::fabs(fs - fs_) > 1e-9);
        if (rateChanged) {
            fs_ = (fs > 0.0) ? fs : 48000.0;
            rebuild_locked(false);
        }
        if (channels < 1) channels = 1;
        if (channels != ch_) {
            ch_ = channels;
            chans_.assign((size_t)ch_, Chan{});
            rebuild_locked(false);
        }
    }

    // 参数热更新：新链从零状态启动，与旧链交叉淡入，不打断音频线程
    void set_settings(const PeqSettings& s) {
        std::lock_guard<std::mutex> lk(mu_);
        s_ = s;
        rebuild_locked(true);
    }

    PeqSettings settings() const {
        std::lock_guard<std::mutex> lk(mu_);
        return s_;
    }

    void set_bypass(bool b) {
        std::lock_guard<std::mutex> lk(mu_);
        s_.bypass = b;
        rebuild_locked(true);
    }
    bool bypass() const { std::lock_guard<std::mutex> lk(mu_); return s_.bypass; }

    // 是否真的会改变声音（用于「逐位直通」路径判定：全 0dB / bypass 不算处理）
    bool has_effect() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (s_.bypass) return false;
        if (std::fabs(s_.preampDb) > 1e-9) return true;
        for (const auto& b : s_.bands)
            if (b.enabled && std::fabs(b.gainDb) > 1e-9) return true;
        return false;
    }

    double sample_rate() const { std::lock_guard<std::mutex> lk(mu_); return fs_; }

    // 当前设置下的总响应（dB，含 preamp）
    double response_db(double freq) const {
        std::lock_guard<std::mutex> lk(mu_);
        return peq_response_db(s_, fs_, freq);
    }

    // 处理一个声道的 double 缓冲（原地）。音频线程调用；无锁竞争时几乎零开销。
    void process(size_t ch, double* buf, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!buf || n == 0 || ch >= chans_.size()) return;
        Chan& c = chans_[ch];
        bool fading = c.fadeLeft > 0;
        // 快速路径：无交叉淡入且当前链为空、增益 1 → 直通
        if (!fading && c.cur.empty() && std::fabs(c.gainCur - 1.0) < 1e-12) return;
        for (size_t i = 0; i < n; ++i) {
            const float x = (float)buf[i];
            float y;
            if (fading) {
                const float a = run_chain(c.prev, x) * (float)c.gainPrev;
                const float b = run_chain(c.cur, x) * (float)c.gainCur;
                const float t = 1.0f - (float)c.fadeLeft / (float)c.fadeLen;
                y = a * (1.0f - t) + b * t;
                if (--c.fadeLeft <= 0) { fading = false; c.prev.clear(); }
            } else {
                y = run_chain(c.cur, x) * (float)c.gainCur;
            }
            buf[i] = (double)y;
        }
    }

private:
    struct Chan {
        std::vector<PeqBiquadF32> cur, prev;   // 当前链 / 交叉淡入期间的旧链
        double gainCur = 1.0, gainPrev = 1.0;
        int    fadeLeft = 0, fadeLen = 1;
    };

    static inline float run_chain(std::vector<PeqBiquadF32>& chain, float x) {
        for (auto& q : chain) x = q.process(x);
        return x;
    }

    void rebuild_locked(bool crossfade) {
        const std::vector<PeqCoeffs> coeffs = peq_design(s_, fs_);
        const double gain = peq_gain_linear(s_);
        if (chans_.empty()) chans_.assign((size_t)ch_, Chan{});
        for (auto& c : chans_) {
            std::vector<PeqBiquadF32> next(coeffs.size());
            for (size_t i = 0; i < coeffs.size(); ++i) next[i].set(coeffs[i]);
            if (crossfade) {
                // 旧链连同其滤波器状态整体挪到 prev，保证淡入期间输出连续
                c.prev = std::move(c.cur);
                c.gainPrev = c.gainCur;
                c.cur = std::move(next);
                c.gainCur = gain;
                c.fadeLen = fadeLen_;
                c.fadeLeft = fadeLen_;
            } else {
                c.cur = std::move(next);
                c.gainCur = gain;
                c.prev.clear();
                c.fadeLeft = 0;
            }
        }
    }

    mutable std::mutex mu_;
    PeqSettings s_ = peq_flat_settings();
    double fs_ = 48000.0;
    int    ch_ = 2;
    int    fadeLen_ = 128;    // 交叉淡入样本数（约 2.9ms @44.1k / 2.7ms @48k）
    std::vector<Chan> chans_;
};

} // namespace rta::dsp
