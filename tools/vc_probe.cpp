// tools/vc_probe.cpp — 版本对比（version_compare.h）置信度/稳定性审查探针
// 用法:
//   vc_probe gen <dir>                  生成受控素材（WAV，44.1k/16 为主；另出 48k/24、96k/24、单声道）
//   vc_probe run <a> <b> [c...]         对比并把每版指标/维度扣分/总分（17 位有效数字）打出来
//   vc_probe repeat <k> <a> <b> [...]   同一批文件连跑 k 次，检查分数/排名是否逐位一致
// 纪律：只读/只写本地文件，不开任何音频设备（铁律 1）。
#include "analysis/version_compare.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>

using namespace rta::analysis;
using rta::dsp::kPi;
namespace fsx = std::filesystem;

static void wav_write(const std::string& path, const std::vector<std::vector<double>>& ch,
                      uint32_t fs, int bits) {
    const size_t nch = ch.size();
    if (nch == 0) return;
    const size_t n = ch[0].size();
    const int bytes = bits / 8;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    const uint32_t dataBytes = (uint32_t)(n * nch * (size_t)bytes);
    auto u32 = [&](uint32_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF));
                                 f.put((char)((v >> 16) & 0xFF)); f.put((char)((v >> 24) & 0xFF)); };
    auto u16 = [&](uint16_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF)); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16((uint16_t)nch); u32(fs);
    u32(fs * (uint32_t)nch * (uint32_t)bytes); u16((uint16_t)(nch * (size_t)bytes)); u16((uint16_t)bits);
    f.write("data", 4); u32(dataBytes);
    const double scale = (bits == 16) ? 32768.0 : (bits == 24 ? 8388608.0 : 2147483648.0);
    const long long vmax = (bits == 16) ? 32767 : (bits == 24 ? 8388607 : 2147483647LL);
    std::vector<char> row(nch * (size_t)bytes);
    for (size_t i = 0; i < n; ++i) {
        for (size_t c = 0; c < nch; ++c) {
            long long v = (long long)std::llround(ch[c][i] * scale);
            v = std::max<long long>(-(vmax + 1), std::min<long long>(vmax, v));
            for (int b = 0; b < bytes; ++b) row[c * (size_t)bytes + (size_t)b] = (char)((v >> (8 * b)) & 0xFF);
        }
        f.write(row.data(), (std::streamsize)row.size());
    }
}

// 合成「音乐」：多音 + 慢包络 + 高频乐音 + −60 dBFS 白噪（截止检测需要宽带本底）
static std::vector<std::vector<double>> make_music(size_t n, double fs, double peak,
                                                   int song = 0, double leadSec = 0.0) {
    std::vector<std::vector<double>> ch(2, std::vector<double>(n, 0.0));
    unsigned long long s = 424242ull + 7919ull * (unsigned long long)(song + 1);
    auto uni = [&]() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                       return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0; };
    auto gauss = [&]() { const double u1 = std::max(uni(), 1e-12), u2 = uni();
                         return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2); };
    const size_t lead = (size_t)std::llround(leadSec * fs);
    // 不同 song ⟹ 不同基频/速度/和声，用来做「不同录音」的拒绝率测试
    const double base = (song == 0) ? 196.0 : (song == 1 ? 262.0 : (song == 2 ? 330.0 : 147.0));
    const double rate = (song == 0) ? 0.21 : (song == 1 ? 0.37 : (song == 2 ? 0.13 : 0.55));
    const double third = (song == 0) ? 1.5 : (song == 1 ? 1.25 : (song == 2 ? 1.3333 : 1.2));
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / fs;
        if (i < lead) { ch[0][i] = 0.0; ch[1][i] = 0.0; continue; }
        const double env = 0.45 + 0.55 * std::fabs(std::sin(2.0 * kPi * rate * t));
        const double l = env * (0.34 * std::sin(2.0 * kPi * base * t)
                              + 0.22 * std::sin(2.0 * kPi * base * third * t)
                              + 0.12 * std::sin(2.0 * kPi * base * 6.0 * t)
                              + 0.05 * std::sin(2.0 * kPi * 4410.0 * t));
        const double rr = env * (0.30 * std::sin(2.0 * kPi * base * t + 0.4)
                               + 0.20 * std::sin(2.0 * kPi * base * (third + 0.5) * t)
                               + 0.10 * std::sin(2.0 * kPi * base * 9.0 * t));
        const double hf = 0.020 * std::sin(2.0 * kPi * 11800.0 * t)
                        + 0.015 * std::sin(2.0 * kPi * 16400.0 * t)
                        + 0.012 * std::sin(2.0 * kPi * 19600.0 * t)
                        + 0.008 * std::sin(2.0 * kPi * 21500.0 * t);
        ch[0][i] = l + hf + 1e-3 * gauss();
        ch[1][i] = rr + 0.9 * hf + 0.8 * 1e-3 * gauss();
    }
    double mx = 0.0;
    for (const auto& c : ch) for (double v : c) mx = std::max(mx, std::fabs(v));
    if (mx > 0.0) for (auto& c : ch) for (double& v : c) v *= peak / mx;
    return ch;
}

static void scale_to_peak(std::vector<std::vector<double>>& ch, double peak) {
    double mx = 0.0;
    for (const auto& c : ch) for (double v : c) mx = std::max(mx, std::fabs(v));
    if (mx > 0.0) for (auto& c : ch) for (double& v : c) v *= peak / mx;
}

static int gen(const std::string& dir) {
    fsx::create_directories(dir);
    const double fs44 = 44100.0, fs48 = 48000.0, fs96 = 96000.0;
    const size_t n60 = (size_t)(60.0 * fs44);
    const double peak = 0.89;   // −1 dBFS
    auto music = make_music(n60, fs44, peak, 0);
    wav_write(dir + "\\music_44_16.wav", music, 44100, 16);
    wav_write(dir + "\\music_44_24.wav", music, 44100, 24);

    // 单声道（同一内容取 (L+R)/2）
    { auto m = music; std::vector<std::vector<double>> mono(1, std::vector<double>(n60, 0.0));
      for (size_t i = 0; i < n60; ++i) mono[0][i] = 0.5 * (m[0][i] + m[1][i]);
      wav_write(dir + "\\music_mono_44_16.wav", mono, 44100, 16); }

    // 同一内容 48 kHz（模拟「同一母带存成不同采样率」）
    { auto m48 = make_music((size_t)(60.0 * fs48), fs48, peak, 0);
      wav_write(dir + "\\music_48_24.wav", m48, 48000, 24); }

    // 24bit/96k「母带」
    { auto m96 = make_music((size_t)(60.0 * fs96), fs96, peak, 0);
      wav_write(dir + "\\master_96_24.wav", m96, 96000, 24); }

    // 前导静音 0.5 / 2 / 5 s（同一内容）
    for (double lead : {0.5, 2.0, 5.0}) {
        auto m = make_music(n60 + (size_t)(lead * fs44), fs44, peak, 0, lead);
        char nm[64]; std::snprintf(nm, sizeof(nm), "music_lead%03d.wav", (int)std::lround(lead * 100));
        wav_write(dir + "\\" + nm, m, 44100, 16);
    }

    // 前 60% 截断
    { auto m = music; std::vector<std::vector<double>> t(2, std::vector<double>((size_t)(n60 * 0.6), 0.0));
      for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < t[c].size(); ++i) t[c][i] = m[c][i];
      wav_write(dir + "\\music_trunc60.wav", t, 44100, 16); }

    // 前后各裁 1 s
    { std::vector<std::vector<double>> t(2, std::vector<double>(n60 - (size_t)(2 * fs44), 0.0));
      for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < t[c].size(); ++i) t[c][i] = music[c][i + (size_t)fs44];
      wav_write(dir + "\\music_trim1s.wav", t, 44100, 16); }

    // A: +0.5 dB 削波
    { auto m = music; const double g = std::pow(10.0, 0.5 / 20.0);
      for (auto& c : m) for (double& v : c) v = std::clamp(v * g, -1.0, 1.0);
      wav_write(dir + "\\clip_plus0p5db.wav", m, 44100, 16); }

    // B: +1% 三次谐波失真（y = x + 0.01·x³，再回原峰值，避免引入削波）
    { auto m = music; for (auto& c : m) for (double& v : c) v = v + 0.01 * v * v * v;
      scale_to_peak(m, peak); wav_write(dir + "\\dist3_1pct.wav", m, 44100, 16); }

    // C: + 白噪 −60 dBFS（RMS 1e-3）
    { auto m = music; unsigned long long s = 1234567ull;
      auto uni = [&]() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                         return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0; };
      auto gauss = [&]() { const double u1 = std::max(uni(), 1e-12), u2 = uni();
                           return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2); };
      for (auto& c : m) for (double& v : c) v += 1e-3 * gauss();
      wav_write(dir + "\\noise_m60.wav", m, 44100, 16); }

    // G: 同一母带两种压限（高 DR vs 低 DR）—— 都用 tanh 软限幅，峰值归一到同一电平
    { auto m = music;
      for (auto& c : m) for (double& v : c) { const double k = 1.0; v = std::tanh(v * k) / std::tanh(k); }
      scale_to_peak(m, peak);
      wav_write(dir + "\\dr_high.wav", m, 44100, 16); }
    { auto m = music;
      for (auto& c : m) for (double& v : c) { const double k = 6.0; v = std::tanh(v * k) / std::tanh(k); }
      scale_to_peak(m, peak);
      wav_write(dir + "\\dr_low.wav", m, 44100, 16); }

    // 不同录音 song1/2/3
    for (int sg = 1; sg <= 3; ++sg) {
        auto m = make_music(n60, fs44, peak, sg);
        char nm[64]; std::snprintf(nm, sizeof(nm), "song%d_44_16.wav", sg);
        wav_write(dir + "\\" + nm, m, 44100, 16);
    }
    // 全静音
    { std::vector<std::vector<double>> z(2, std::vector<double>((size_t)(10 * fs44), 0.0));
      wav_write(dir + "\\silent_10s.wav", z, 44100, 16); }
    printf("gen ok -> %s\n", dir.c_str());
    return 0;
}

// ---- 瞬态丰富（接近真实音乐）的素材：不规则的鼓点 + 非周期包络 + 宽带本底 ----
// 为什么需要它：上面那份 make_music 的慢包络是单频正弦（0.21 Hz），200 Hz 包络高度周期化，
//   互相关在整周期处几乎同高 ⟹ 对齐测试会被「周期滑动」支配，测不出真实算法行为。
//   真实曲子在 5 ms 包络上有鼓点/换气/咬字，相关峰尖锐。
static std::vector<std::vector<double>> make_aper(size_t n, double fs, double peak,
                                                 int song = 0, double leadSec = 0.0) {
    std::vector<std::vector<double>> ch(2, std::vector<double>(n, 0.0));
    unsigned long long s = 20261027ull + 104729ull * (unsigned long long)(song + 1);
    auto uni = [&]() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                       return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0; };
    auto gauss = [&]() { const double u1 = std::max(uni(), 1e-12), u2 = uni();
                         return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2); };
    const size_t lead = (size_t)std::llround(leadSec * fs);
    struct Hit { size_t at; double gL, gR, decay; };
    std::vector<Hit> hits;
    { size_t t = (size_t)(0.05 * fs);
      while (t < n) {
          Hit h; h.at = t; h.gL = 0.25 + 0.75 * uni(); h.gR = 0.25 + 0.75 * uni();
          h.decay = 30.0 + 60.0 * uni();
          hits.push_back(h);
          t += (size_t)((0.28 + 0.57 * uni()) * fs);
      } }
    const double base = (song == 0) ? 98.0 : (song == 1 ? 174.6 : (song == 2 ? 261.6 : 329.6));
    const double slow1 = (song == 0) ? 0.37 : (song == 1 ? 0.23 : (song == 2 ? 0.51 : 0.17));
    const double slow2 = (song == 0) ? 0.11 : (song == 1 ? 0.29 : (song == 2 ? 0.07 : 0.43));
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / fs;
        if (i < lead) continue;
        const double env = 0.30 + 0.35 * std::fabs(std::sin(2.0 * kPi * slow1 * t))
                                 + 0.20 * std::fabs(std::sin(2.0 * kPi * slow2 * t + 1.1));
        const double tone = env * (0.30 * std::sin(2.0 * kPi * base * t)
                                 + 0.18 * std::sin(2.0 * kPi * base * 2.0 * t)
                                 + 0.10 * std::sin(2.0 * kPi * base * 3.0 * t)
                                 + 0.06 * std::sin(2.0 * kPi * base * 5.0 * t));
        const double hf = 0.015 * std::sin(2.0 * kPi * 13100.0 * t)
                        + 0.012 * std::sin(2.0 * kPi * 17800.0 * t)
                        + 0.008 * std::sin(2.0 * kPi * 20400.0 * t);
        ch[0][i] += tone + hf + 8e-4 * gauss();
        ch[1][i] += 0.92 * tone + 0.9 * hf + 8e-4 * gauss();
    }
    for (const Hit& h : hits) {
        for (size_t k = 0; k < (size_t)(0.22 * fs) && h.at + k < n; ++k) {
            const double tt = (double)k / fs;
            const double e = std::exp(-h.decay * tt) * h.gL;
            const double burst = e * (0.7 * gauss() * 0.25 + 0.5 * std::sin(2.0 * kPi * 62.0 * tt));
            ch[0][h.at + k] += burst;
            ch[1][h.at + k] += burst * h.gR;
        }
    }
    scale_to_peak(ch, peak);
    return ch;
}

static int gen2(const std::string& dir) {
    fsx::create_directories(dir);
    const double fs = 44100.0, peak = 0.89;
    const size_t n60 = (size_t)(60.0 * fs);
    auto a = make_aper(n60, fs, peak, 0);
    wav_write(dir + "\\aper_base.wav", a, 44100, 16);
    for (double lead : {0.5, 2.0, 5.0}) {
        auto m = make_aper(n60 + (size_t)(lead * fs), fs, peak, 0, lead);
        char nm[64]; std::snprintf(nm, sizeof(nm), "aper_lead%03d.wav", (int)std::lround(lead * 100));
        wav_write(dir + "\\" + nm, m, 44100, 16);
    }
    { std::vector<std::vector<double>> t(2, std::vector<double>((size_t)(n60 * 0.6), 0.0));
      for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < t[c].size(); ++i) t[c][i] = a[c][i];
      wav_write(dir + "\\aper_trunc60.wav", t, 44100, 16); }
    { std::vector<std::vector<double>> t(2, std::vector<double>(n60 - (size_t)(2 * fs), 0.0));
      for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < t[c].size(); ++i) t[c][i] = a[c][i + (size_t)fs];
      wav_write(dir + "\\aper_trim1s.wav", t, 44100, 16); }
    for (int sg = 1; sg <= 3; ++sg) {
        auto m = make_aper(n60, fs, peak, sg);
        char nm[64]; std::snprintf(nm, sizeof(nm), "aper_song%d.wav", sg);
        wav_write(dir + "\\" + nm, m, 44100, 16); }
    printf("gen2 ok -> %s\n", dir.c_str());
    return 0;
}

// ---- gen3：aper 素材的受控变体（对照组 A~G 用；每组只改一个变量）----
static int gen3(const std::string& dir) {
    fsx::create_directories(dir);
    const double fs = 44100.0, peak = 0.89;
    const size_t n60 = (size_t)(60.0 * fs);
    auto a = make_aper(n60, fs, peak, 0);
    // A: +0.5 dB 削波
    { auto m = a; const double g = std::pow(10.0, 0.5 / 20.0);
      for (auto& c : m) for (double& v : c) v = std::clamp(v * g, -1.0, 1.0);
      wav_write(dir + "\\aper_clip05.wav", m, 44100, 16); }
    // B: +1% 三次谐波失真
    { auto m = a; for (auto& c : m) for (double& v : c) v = v + 0.01 * v * v * v;
      scale_to_peak(m, peak); wav_write(dir + "\\aper_dist3.wav", m, 44100, 16); }
    // C: + 白噪 −60 dBFS
    { auto m = a; unsigned long long s = 987654321ull;
      auto uni = [&]() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                         return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0; };
      auto gauss = [&]() { const double u1 = std::max(uni(), 1e-12), u2 = uni();
                           return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2); };
      for (auto& c : m) for (double& v : c) v += 1e-3 * gauss();
      wav_write(dir + "\\aper_noise60.wav", m, 44100, 16); }
    // G: 同一母带两种压限
    { auto m = a; for (auto& c : m) for (double& v : c) v = std::tanh(v * 1.0) / std::tanh(1.0);
      scale_to_peak(m, peak); wav_write(dir + "\\aper_drhigh.wav", m, 44100, 16); }
    { auto m = a; for (auto& c : m) for (double& v : c) v = std::tanh(v * 6.0) / std::tanh(6.0);
      scale_to_peak(m, peak); wav_write(dir + "\\aper_drlow.wav", m, 44100, 16); }
    // F 的母带侧：同内容 96 kHz / 24-bit
    { auto m96 = make_aper((size_t)(60.0 * 96000.0), 96000.0, peak, 0);
      wav_write(dir + "\\aper_master96_24.wav", m96, 96000, 24); }
    // 24-bit 容器（同内容）
    wav_write(dir + "\\aper_24.wav", a, 44100, 24);
    printf("gen3 ok -> %s\n", dir.c_str());
    return 0;
}

// ---- gen4：满幅素材（削波/失真对照组必须从接近 0 dBFS 的电平起做，否则 +0.5 dB 削不到）----
static int gen4(const std::string& dir) {
    fsx::create_directories(dir);
    const double fs = 44100.0;
    const size_t n60 = (size_t)(60.0 * fs);
    auto a = make_aper(n60, fs, 0.99, 0);          // −0.09 dBFS
    wav_write(dir + "\\hot_base.wav", a, 44100, 16);
    { auto m = a; const double g = std::pow(10.0, 0.5 / 20.0);   // +0.5 dB ⟹ 峰值 1.049 越界
      for (auto& c : m) for (double& v : c) v = std::clamp(v * g, -1.0, 1.0);
      wav_write(dir + "\\hot_clip05.wav", m, 44100, 16); }
    { auto m = a; for (auto& c : m) for (double& v : c) v = v + 0.01 * v * v * v;
      scale_to_peak(m, 0.99); wav_write(dir + "\\hot_dist1.wav", m, 44100, 16); }
    { auto m = a; for (auto& c : m) for (double& v : c) v = v + 0.05 * v * v * v;
      scale_to_peak(m, 0.99); wav_write(dir + "\\hot_dist5.wav", m, 44100, 16); }
    { auto m = a; for (auto& c : m) for (double& v : c) v = std::tanh(v * 8.0) / std::tanh(8.0);
      scale_to_peak(m, 0.99); wav_write(dir + "\\hot_limiter.wav", m, 44100, 16); }
    printf("gen4 ok -> %s\n", dir.c_str());
    return 0;
}

// ---- lagdump：把对齐过程摊开（包络长度、FFT 峰值、各候选延迟的重叠相关）----
static int lagdump(const std::string& fa, const std::string& fb) {
    auto load_env = [](const std::string& p, std::vector<float>& env, double& envHz,
                       uint32_t& fs, size_t& frames) -> bool {
        PcmFile pf; std::string why;
        if (!pf.open(p, &why)) { printf("open fail %s: %s\n", p.c_str(), why.c_str()); return false; }
        fs = pf.rate(); frames = pf.totalFrames();
        env = vcdetail::envelope_of(pf, 65536, &envHz);
        // 与 compare_versions 相同的 200 Hz 重采样
        std::vector<float> out;
        const double targetHz = 200.0;
        for (double t = 0.0; t < (double)env.size() / envHz; t += 1.0 / targetHz) {
            const double fpos = t * envHz;
            const size_t i0 = (size_t)fpos;
            const size_t i1 = std::min(env.size() - 1, i0 + 1);
            const double a = fpos - (double)i0;
            out.push_back((float)((1.0 - a) * env[i0] + a * env[i1]));
        }
        env.swap(out);
        return true;
    };
    std::vector<float> ea, eb; double ha = 0, hb = 0; uint32_t fa_hz = 0, fb_hz = 0; size_t na = 0, nb = 0;
    if (!load_env(fa, ea, ha, fa_hz, na)) return 1;
    if (!load_env(fb, eb, hb, fb_hz, nb)) return 1;
    printf("A: fs=%u frames=%zu env=%zu (%.3f s)\n", fa_hz, na, ea.size(), (double)ea.size() / 200.0);
    printf("B: fs=%u frames=%zu env=%zu (%.3f s)\n", fb_hz, nb, eb.size(), (double)eb.size() / 200.0);
    long long lag = 0; double corr = 0.0;
    vcdetail::best_lag(ea, eb, &lag, &corr);
    printf("FFT best_lag=%lld (%.3f s) rawCorr=%.6f -> lagSec=%.4f\n",
           lag, (double)lag / 200.0, corr, -(double)lag / 200.0);
    printf("overlap_corr 曲线（秒 : 相关）:\n");
    for (double sec = -6.0; sec <= 12.0; sec += 0.5) {
        const long long L = (long long)std::llround(sec * 200.0);
        const double c = vcdetail::overlap_corr(ea, eb, L);
        printf("  %+6.2f : %8.5f%s\n", sec, c, (L == lag) ? "   <== FFT peak" : "");
    }
    // 逐 5 ms 精细扫描 ±0.2 s 找真正的局部峰（若真峰不在 FFT 峰附近，说明 FFT 峰错）
    double bestC = -2; double bestSec = 0;
    for (long long L = -2400; L <= 2400; ++L) {
        const double c = vcdetail::overlap_corr(ea, eb, L);
        if (c > bestC) { bestC = c; bestSec = (double)L / 200.0; }
    }
    printf("±12 s 穷举峰: %.4f s (corr=%.6f)\n", bestSec, bestC);
    return 0;
}

// ---- diff2：逐样本比对两个文件在给定偏移下的差异（验证"纯移位"是否成立）----
static int diff2(const std::string& fa, const std::string& fb, double shiftSec) {
    PcmFile a, b; std::string w1, w2;
    if (!a.open(fa, &w1)) { printf("open fail A: %s\n", w1.c_str()); return 1; }
    if (!b.open(fb, &w2)) { printf("open fail B: %s\n", w2.c_str()); return 1; }
    printf("A fs=%u ch=%d frames=%zu | B fs=%u ch=%d frames=%zu | shift=%.3f s\n",
           a.rate(), a.channels(), a.totalFrames(), b.rate(), b.channels(), b.totalFrames(), shiftSec);
    const size_t off = (size_t)std::llround(shiftSec * (double)a.rate());
    const size_t n = std::min(a.totalFrames() - off, b.totalFrames());
    rta::audio::AudioBuffer ca, cb;
    const size_t blk = 65536;
    double mx = 0.0, sum = 0.0; size_t cnt = 0;
    for (size_t pos = 0; pos < n; pos += blk) {
        const size_t k = std::min(blk, n - pos);
        const size_t ga = a.read(off + pos, k, ca);
        const size_t gb = b.read(pos, k, cb);
        if (ga != k || gb != k) { printf("short read at %zu: %zu/%zu\n", pos, ga, gb); break; }
        for (size_t i = 0; i < k; ++i)
            for (int c = 0; c < a.channels(); ++c) {
                const double d = ca.sample(i, c) - cb.sample(i, c);
                mx = std::max(mx, std::fabs(d)); sum += d * d; ++cnt;
            }
    }
    printf("max|dA-dB| = %.9g   rms=%.9g   frames=%zu\n", mx, std::sqrt(sum / (double)std::max<size_t>(1, cnt)), n);
    return 0;
}

// ---- envcmp：直接比两个包络在给定点偏移下的 Pearson + 前若干点数值 ----
static int envcmp(const std::string& fa, const std::string& fb, long long lagPts) {
    auto load = [](const std::string& p, std::vector<float>& out, double& dur) -> bool {
        PcmFile pf; std::string why;
        if (!pf.open(p, &why)) { printf("open fail: %s\n", why.c_str()); return false; }
        double envHz = 200.0;
        std::vector<float> env = vcdetail::envelope_of(pf, 65536, &envHz);
        out.clear();
        for (double t = 0.0; t < (double)env.size() / envHz; t += 1.0 / 200.0) {
            const double fpos = t * envHz;
            const size_t i0 = (size_t)fpos;
            const size_t i1 = std::min(env.size() - 1, i0 + 1);
            const double a = fpos - (double)i0;
            out.push_back((float)((1.0 - a) * env[i0] + a * env[i1]));
        }
        dur = (double)env.size() / envHz;
        printf("  raw env=%zu pts, envHz=%.4f, dur=%.4f s, resampled=%zu\n",
               env.size(), envHz, dur, out.size());
        return true;
    };
    std::vector<float> A, B; double da = 0, db = 0;
    printf("A: %s\n", fa.c_str()); if (!load(fa, A, da)) return 1;
    printf("B: %s\n", fb.c_str()); if (!load(fb, B, db)) return 1;
    // 直接 Pearson：A[lagPts + i] vs B[i]
    double sa = 0, sb = 0; long long cnt = 0;
    for (long long i = 0; i + lagPts < (long long)A.size() && i < (long long)B.size(); ++i) {
        sa += A[(size_t)(i + lagPts)]; sb += B[(size_t)i]; ++cnt;
    }
    const double ma = sa / (double)cnt, mb = sb / (double)cnt;
    double cov = 0, va = 0, vb = 0;
    for (long long i = 0; i + lagPts < (long long)A.size() && i < (long long)B.size(); ++i) {
        const double x = A[(size_t)(i + lagPts)] - ma, y = B[(size_t)i] - mb;
        cov += x * y; va += x * x; vb += y * y;
    }
    printf("direct Pearson at lagPts=%lld: %.6f (n=%lld)\n", lagPts,
           cov / std::sqrt(std::max(1e-30, va * vb)), cnt);
    printf("样本对比（A[lag+i] vs B[i]）:\n");
    for (long long i = 3000; i < 3008; ++i)
        printf("  i=%lld  A=%.6f  B=%.6f\n", i,
               (double)A[(size_t)(i + lagPts)], (double)B[(size_t)i]);
    return 0;
}

// ---- envsm：验证「包络平滑能否救回对齐」（1/3/5/7/9/11 点移动平均 + 穷举峰）----
static std::vector<float> smooth_env(const std::vector<float>& x, int w) {
    if (w <= 1) return x;
    std::vector<float> o(x.size(), 0.0f);
    const int h = w / 2;
    for (long long i = 0; i < (long long)x.size(); ++i) {
        double s = 0.0; int n = 0;
        for (int k = -h; k <= h; ++k) {
            const long long j = i + k;
            if (j < 0 || j >= (long long)x.size()) continue;
            s += x[(size_t)j]; ++n;
        }
        o[(size_t)i] = (float)(s / std::max(1, n));
    }
    return o;
}
static int envsm(const std::string& fa, const std::string& fb) {
    auto load = [](const std::string& p, std::vector<float>& out) -> bool {
        PcmFile pf; std::string why;
        if (!pf.open(p, &why)) { printf("open fail: %s\n", why.c_str()); return false; }
        double envHz = 200.0;
        std::vector<float> env = vcdetail::envelope_of(pf, 65536, &envHz);
        out.clear();
        for (double t = 0.0; t < (double)env.size() / envHz; t += 1.0 / 200.0) {
            const double fpos = t * envHz;
            const size_t i0 = (size_t)fpos;
            const size_t i1 = std::min(env.size() - 1, i0 + 1);
            const double a = fpos - (double)i0;
            out.push_back((float)((1.0 - a) * env[i0] + a * env[i1]));
        }
        return true;
    };
    std::vector<float> A, B;
    if (!load(fa, A) || !load(fb, B)) return 1;
    printf("%-4s %-14s %-14s %-14s\n", "w", "corr@trueLag", "brutePeak(s)", "corrAtPeak");
    for (int w : {1, 3, 5, 7, 9, 11, 15}) {
        std::vector<float> a = smooth_env(A, w), b = smooth_env(B, w);
        long long bestL = 0; double bestC = -2;
        for (long long L = -2400; L <= 2400; ++L) {
            const double c = vcdetail::overlap_corr(a, b, L);
            if (c > bestC) { bestC = c; bestL = L; }
        }
        // FFT 法在同一平滑包络上的结果
        long long fl = 0; double fc = 0;
        vcdetail::best_lag(a, b, &fl, &fc);
        printf("w=%-3d FFT峰=%+7.3f s 穷举峰=%+7.3f s corr=%6.4f\n", w,
               -(double)fl / 200.0, (double)bestL / 200.0, bestC);
    }
    return 0;
}

// ---- gen5：正确的"前导静音/单声道折叠"素材（在成品样本上做，保证内容逐样本相同）----
static int gen5(const std::string& dir) {
    fsx::create_directories(dir);
    const double fs = 44100.0;
    const size_t n60 = (size_t)(60.0 * fs);
    auto a = make_aper(n60, fs, 0.89, 0);
    auto prepend = [&](double leadSec) {
        const size_t lead = (size_t)std::llround(leadSec * fs);
        std::vector<std::vector<double>> o(2, std::vector<double>(lead + n60, 0.0));
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < n60; ++i) o[c][lead + i] = a[c][i];
        return o;
    };
    for (double lead : {0.5, 2.0, 3.0, 5.0}) {
        char nm[64]; std::snprintf(nm, sizeof(nm), "pp_lead%03d.wav", (int)std::lround(lead * 100));
        wav_write(dir + "\\" + nm, prepend(lead), 44100, 16);
    }
    // 单声道折叠（与 base 同内容）
    { std::vector<std::vector<double>> m(1, std::vector<double>(n60, 0.0));
      for (size_t i = 0; i < n60; ++i) m[0][i] = 0.5 * (a[0][i] + a[1][i]);
      wav_write(dir + "\\pp_mono.wav", m, 44100, 16); }
    printf("gen5 ok -> %s\n", dir.c_str());
    return 0;
}

static const char* bn(const std::string& p) {
    const size_t k = p.find_last_of("\\/");
    return (k == std::string::npos) ? p.c_str() : p.c_str() + k + 1;
}

static void dump(const VersionCompare& r) {
    printf("RESULT ok=%d same=%d corr=%.17g window=%.6f versions=%zu\n",
           (int)r.ok, (int)r.sameTrack, r.sameTrackCorr, r.windowSec, r.versions.size());
    if (!r.ok) { printf("FAILED: %s\n", r.error.c_str()); return; }
    for (size_t i = 0; i < r.versions.size(); ++i) {
        const VersionData& v = r.versions[i];
        printf("  %s %-26s fs=%u ch=%d bits=%d dur=%.3f lag=%+.4f envCorr=%.6f "
               "cutoff=%.1f conf=%.3f lossy=%d analyzed=%d err=%s\n",
               v.label.c_str(), bn(v.path), v.fs, v.channels, v.bits, v.durationSec,
               v.lagSec, v.envCorr, v.spectralCutoffHz, v.cutoffConfidence,
               (int)v.lossySuspected, (int)v.analyzed, v.error.c_str());
        if (!v.cutoffReason.empty()) printf("      reason: %s\n", v.cutoffReason.c_str());
    }
    printf("指标行（comparable / 值）:\n");
    for (const MetricRow& row : r.rows) {
        printf("  %-16s cmp=%d best=%zu %s", row.key.c_str(), (int)row.comparable, row.bestIndex,
               row.comparable ? "" : ("<< " + row.why).c_str());
        for (double x : row.values) printf(" %.17g", x);
        printf("\n");
    }
    printf("维度扣分（penalty）/ 折算分:\n");
    for (const auto& d : r.dims) {
        printf("  %-14s w=%.2f pen:", d.key.c_str(), d.weight);
        for (double p : d.penalty) printf(" %.6f", p);
        printf("  |score:");
        for (double s : d.score) printf(" %.4f", s);
        printf("\n");
    }
    printf("SCORES:");
    for (double s : r.scores) printf(" %.17g", s);
    printf("\nRANKING:");
    for (size_t i : r.ranking) printf(" %zu", i);
    printf("\n");
    if (!r.caveats.empty()) {
        printf("CAVEATS(%zu):\n", r.caveats.size());
        for (const std::string& c : r.caveats) printf("  * %s\n", c.c_str());
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("用法: vc_probe gen <dir> | run <a> <b> [...] | repeat <k> <a> <b> [...]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "gen") return gen(argv[2]);
    if (cmd == "gen2") return gen2(argv[2]);
    if (cmd == "gen3") return gen3(argv[2]);
    if (cmd == "gen4") return gen4(argv[2]);
    if (cmd == "gen5") return gen5(argv[2]);
    if (cmd == "lagdump" && argc >= 4) return lagdump(argv[2], argv[3]);
    if (cmd == "diff2" && argc >= 4) return diff2(argv[2], argv[3], (argc >= 5) ? std::atof(argv[4]) : 0.0);
    if (cmd == "envcmp" && argc >= 4) return envcmp(argv[2], argv[3], (argc >= 5) ? std::atoll(argv[4]) : 0);
    if (cmd == "envsm" && argc >= 4) return envsm(argv[2], argv[3]);
    if (cmd == "run" || cmd == "repeat") {
        int k = 1, first = 2;
        if (cmd == "repeat") { k = std::max(1, std::atoi(argv[2])); first = 3; }
        std::vector<std::string> files;
        for (int i = first; i < argc; ++i) files.push_back(argv[i]);
        std::vector<std::vector<double>> all;
        std::vector<std::vector<size_t>> ranks;
        for (int it = 0; it < k; ++it) {
            CompareOptions opt;
            VersionCompare r = compare_versions(files, opt);
            score_versions(r);
            if (it == 0) dump(r);
            else {
                printf("iter %d SCORES:", it + 1);
                for (double s : r.scores) printf(" %.17g", s);
                printf("\n");
            }
            if (!r.ok) return 1;
            all.push_back(r.scores);
            ranks.push_back(r.ranking);
        }
        if (k > 1) {
            bool same = true;
            for (int it = 1; it < k; ++it)
                if (all[it] != all[0] || ranks[it] != ranks[0]) same = false;
            printf("STABILITY k=%d bitIdentical=%s\n", k, same ? "1" : "0");
            // 逐位差
            for (int it = 1; it < k; ++it)
                for (size_t i = 0; i < all[0].size(); ++i)
                    if (all[it][i] != all[0][i])
                        printf("  DIFF iter%d V%zu: %.17g vs %.17g\n", it + 1, i + 1, all[it][i], all[0][i]);
        }
        return 0;
    }
    printf("未知子命令\n");
    return 2;
}
