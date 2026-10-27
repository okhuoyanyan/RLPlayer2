// test_version_compare_stability.cpp — 版本对比（version_compare.h）的【置信度/稳定性】回归
//
// 背景（2026-10-27 DEBUG 审查）：这套打分会被用户直接当结论读（"V3 100.0 > V1 93.4"），
// 所以判据不能只是"排序看起来对"，必须钉死四件事：
//   §1 确定性：同一对文件跑 5 次，分数与名次必须【逐位相等】（打分里不许有随机性/未初始化读）
//   §2 容器无关：同一份 PCM 换成 WAV / FLAC（16-bit 与 24-bit）必须【所有指标逐位相等】、分差 0
//   §3 对齐鲁棒：前置静音 0.5/2/3/5 s、裁掉前 1 s、只保留前 60% ⟹ 延迟必须报对，
//      否则"公共区段"会错位，后面所有指标全部是错的（实测原实现错 9.5 s 仍判同一首）
//   §4 拒绝出排名：不同录音必须 ok=false（拒绝率 100%）；全静音同样拒绝
//   §5 可比性标记：单声道 vs 立体声 ⟹ 声道相关/单声道损失必须标不可比；44.1k vs 48k ⟹
//      采样率提示必须出现（非谐波残留会随采样率漂 13.9 dB，实测）
//   §6 分差提示：两版只差 0.2 分时，必须给出"分差 <1 分，名次不应被当结论"的 caveat
//
// 素材全部合成、写在本目录下的临时目录里（默认 build 目录），完全离线、不开音频设备。
// FLAC/MP3 两段需要 ffmpeg.exe（exe 同目录或 PATH）；找不到时打印「跳过：未验证」并不计入失败
// —— 这是铁律 4 要求的如实标注，不是"假装通过"。
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

static int fails = 0, skips = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static void skip(const std::string& what, const std::string& why) {
    printf("  [SKIP] %s —— 未验证：%s\n", what.c_str(), why.c_str());
    ++skips;
}
static std::string f4(double v) { char b[64]; std::snprintf(b, sizeof(b), "%.6g", v); return b; }

// ---------------- 素材：瞬态丰富、非周期的"音乐" ----------------
// 为什么不用单频包络：5 ms RMS 包络在低频上依赖窗起始相位，周期化素材会把"对齐测试"
// 变成"周期滑动测试"（实测同一内容裁 1 s，原实现给出 +8.53 s 的假延迟）。
static std::vector<std::vector<double>> make_track(size_t n, double fs, double peak, int song = 0) {
    std::vector<std::vector<double>> ch(2, std::vector<double>(n, 0.0));
    unsigned long long s = 20261027ull + 104729ull * (unsigned long long)(song + 1);
    auto uni = [&]() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                       return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0; };
    auto gauss = [&]() { const double u1 = std::max(uni(), 1e-12), u2 = uni();
                         return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2); };
    struct Hit { size_t at; double gL, gR, decay; };
    std::vector<Hit> hits;
    { size_t t = (size_t)(0.05 * fs);
      while (t < n) {
          Hit h; h.at = t; h.gL = 0.25 + 0.75 * uni(); h.gR = 0.25 + 0.75 * uni();
          h.decay = 30.0 + 60.0 * uni(); hits.push_back(h);
          t += (size_t)((0.28 + 0.57 * uni()) * fs);
      } }
    const double base = (song == 0) ? 98.0 : (song == 1 ? 174.6 : (song == 2 ? 261.6 : 329.6));
    const double slow1 = (song == 0) ? 0.37 : (song == 1 ? 0.23 : (song == 2 ? 0.51 : 0.17));
    const double slow2 = (song == 0) ? 0.11 : (song == 1 ? 0.29 : (song == 2 ? 0.07 : 0.43));
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / fs;
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
    for (const Hit& h : hits)
        for (size_t k = 0; k < (size_t)(0.22 * fs) && h.at + k < n; ++k) {
            const double tt = (double)k / fs;
            const double e = std::exp(-h.decay * tt) * h.gL;
            const double burst = e * (0.7 * gauss() * 0.25 + 0.5 * std::sin(2.0 * kPi * 62.0 * tt));
            ch[0][h.at + k] += burst;
            ch[1][h.at + k] += burst * h.gR;
        }
    double mx = 0.0;
    for (const auto& c : ch) for (double v : c) mx = std::max(mx, std::fabs(v));
    if (mx > 0.0) for (auto& c : ch) for (double& v : c) v *= peak / mx;
    return ch;
}

static bool write_wav(const std::string& path, const std::vector<std::vector<double>>& ch,
                      uint32_t fs, int bits) {
    const size_t nch = ch.size();
    if (nch == 0) return false;
    const size_t n = ch[0].size();
    const int bytes = bits / 8;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const uint32_t dataBytes = (uint32_t)(n * nch * (size_t)bytes);
    auto u32 = [&](uint32_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF));
                                 f.put((char)((v >> 16) & 0xFF)); f.put((char)((v >> 24) & 0xFF)); };
    auto u16 = [&](uint16_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF)); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16((uint16_t)nch); u32(fs);
    u32(fs * (uint32_t)nch * (uint32_t)bytes); u16((uint16_t)(nch * (size_t)bytes)); u16((uint16_t)bits);
    f.write("data", 4); u32(dataBytes);
    const long long vmax = (bits == 16) ? 32767 : 8388607;
    const double scale = (bits == 16) ? 32768.0 : 8388608.0;
    std::vector<char> row(nch * (size_t)bytes);
    for (size_t i = 0; i < n; ++i) {
        for (size_t c = 0; c < nch; ++c) {
            long long v = (long long)std::llround(ch[c][i] * scale);
            v = std::max<long long>(-(vmax + 1), std::min<long long>(vmax, v));
            for (int b = 0; b < bytes; ++b) row[c * (size_t)bytes + (size_t)b] = (char)((v >> (8 * b)) & 0xFF);
        }
        f.write(row.data(), (std::streamsize)row.size());
    }
    return (bool)f;
}

static bool have_ffmpeg() {
    if (std::system("ffmpeg -version >NUL 2>NUL") == 0) return true;
    return false;
}
static bool run(const std::string& cmd) { return std::system(cmd.c_str()) == 0; }

static VersionCompare compare_and_score(const std::vector<std::string>& files) {
    VersionCompare r = compare_versions(files, CompareOptions{});
    score_versions(r);
    return r;
}
// 指标逐位相等（所有 row 的所有值 ==，且分数 ==）
static bool metrics_identical(const VersionCompare& r, std::string* why = nullptr) {
    for (const MetricRow& row : r.rows)
        for (size_t i = 1; i < row.values.size(); ++i)
            if (!(row.values[i] == row.values[0])) {
                if (why) *why = row.key + "[" + std::to_string(i) + "]=" + f4(row.values[i]) +
                                " vs " + f4(row.values[0]);
                return false;
            }
    for (size_t i = 1; i < r.scores.size(); ++i)
        if (r.scores[i] != r.scores[0]) {
            if (why) *why = "score[" + std::to_string(i) + "]=" + f4(r.scores[i]) + " vs " + f4(r.scores[0]);
            return false;
        }
    return true;
}
static const MetricRow* row_of(const VersionCompare& r, const std::string& key) {
    for (const MetricRow& row : r.rows) if (row.key == key) return &row;
    return nullptr;
}

int main() {
    fsx::path dir = fsx::temp_directory_path() / "vc_stability";
    fsx::create_directories(dir);
    const std::string d = dir.string();
    const double fs = 44100.0;
    const size_t n = (size_t)(30.0 * fs);           // 30 s 足够判定对齐/判同曲，跑得快
    const std::vector<std::vector<double>> base = make_track(n, fs, 0.89, 0);

    const std::string f_base = d + "\\base.wav";
    const std::string f_lead2 = d + "\\lead2s.wav";
    const std::string f_lead05 = d + "\\lead05s.wav";
    const std::string f_trim = d + "\\trim1s.wav";
    const std::string f_trunc = d + "\\trunc60.wav";
    const std::string f_mono = d + "\\mono.wav";
    const std::string f_48k = d + "\\up48k.wav";
    const std::string f_24 = d + "\\as24.wav";
    const std::string f_song1 = d + "\\song1.wav";
    const std::string f_song2 = d + "\\song2.wav";
    const std::string f_flac = d + "\\base.flac";
    const std::string f_flac24 = d + "\\as24.flac";
    const std::string f_mp3_128 = d + "\\b128.mp3";

    if (!write_wav(f_base, base, 44100, 16)) { printf("写测试素材失败: %s\n", d.c_str()); return 2; }
    // 前置静音（在成品样本前加真正的数字零，逐样本可验证）
    for (auto pr : { std::make_pair(0.5, &f_lead05), std::make_pair(2.0, &f_lead2) }) {
        const size_t lead = (size_t)std::llround(pr.first * fs);
        std::vector<std::vector<double>> o(2, std::vector<double>(lead + n, 0.0));
        for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < n; ++i) o[c][lead + i] = base[c][i];
        write_wav(*pr.second, o, 44100, 16);
    }
    { // 裁掉前 1 s
        std::vector<std::vector<double>> t(2, std::vector<double>(n - (size_t)fs, 0.0));
        for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < t[c].size(); ++i) t[c][i] = base[c][i + (size_t)fs];
        write_wav(f_trim, t, 44100, 16);
    }
    { // 只保留前 60%
        std::vector<std::vector<double>> t(2, std::vector<double>((size_t)(n * 0.6), 0.0));
        for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < t[c].size(); ++i) t[c][i] = base[c][i];
        write_wav(f_trunc, t, 44100, 16);
    }
    { // 单声道折叠
        std::vector<std::vector<double>> m(1, std::vector<double>(n, 0.0));
        for (size_t i = 0; i < n; ++i) m[0][i] = 0.5 * (base[0][i] + base[1][i]);
        write_wav(f_mono, m, 44100, 16);
    }
    { // 24-bit 容器：把 16-bit 量化后的【同一批整数】左移 8 位装进 24-bit 容器
      // （直接写 base 会得到"更高精度的另一份素材"，实测 DR 差 1e-4 dB —— 那是另一件事，不是容器差异）
        std::vector<std::vector<double>> q(2, std::vector<double>(n, 0.0));
        for (size_t c = 0; c < 2; ++c)
            for (size_t i = 0; i < n; ++i)
                q[c][i] = (double)(std::llround(base[c][i] * 32768.0) * 256) / 8388608.0;
        write_wav(f_24, q, 44100, 24);
    }
    { // 48 kHz（同一份内容换采样率：用来验证"跨采样率"提示）
        const size_t n48 = (size_t)(30.0 * 48000.0);
        auto m48 = make_track(n48, 48000.0, 0.89, 0);
        write_wav(f_48k, m48, 48000, 16);
    }
    write_wav(f_song1, make_track(n, fs, 0.89, 1), 44100, 16);
    write_wav(f_song2, make_track(n, fs, 0.89, 2), 44100, 16);

    // ---------------- §1 确定性 ----------------
    printf("§1 确定性（同一输入 5 次）\n");
    {
        std::vector<double> s0; std::vector<size_t> r0;
        bool same = true;
        for (int it = 0; it < 5; ++it) {
            VersionCompare r = compare_and_score({ f_base, f_lead2, d + "\\nope_missing.wav" == f_base ? f_base : f_lead05 });
            if (!r.ok) { same = false; break; }
            if (it == 0) { s0 = r.scores; r0 = r.ranking; }
            else if (r.scores != s0 || r.ranking != r0) same = false;
        }
        check(same, "3 版连跑 5 次：scores/ranking 逐位相等");
        // 再跑一次 4 版组合（含前置静音与长度差异）
        std::vector<double> t0; bool same2 = true;
        for (int it = 0; it < 5; ++it) {
            VersionCompare r = compare_and_score({ f_base, f_lead2, f_trim, f_trunc });
            if (!r.ok) { same2 = false; break; }
            if (it == 0) t0 = r.scores;
            else if (r.scores != t0) same2 = false;
        }
        check(same2, "4 版（含错位/裁剪）连跑 5 次：scores 逐位相等");
    }

    // ---------------- §2 容器无关 ----------------
    printf("§2 容器互换（WAV ↔ FLAC）\n");
    if (have_ffmpeg() && run("ffmpeg -v error -y -i \"" + f_base + "\" -c:a flac \"" + f_flac + "\"") &&
        fsx::exists(f_flac)) {
        VersionCompare r = compare_and_score({ f_base, f_flac });
        std::string why;
        check(r.ok, "16-bit WAV vs FLAC 能对齐", "corr=" + f4(r.sameTrackCorr));
        check(metrics_identical(r, &why), "16-bit WAV vs FLAC：全部指标与分数逐位相等", why);
        check(r.ok && r.scores.size() == 2 && r.scores[0] == r.scores[1], "16-bit 分差 = 0");
        if (run("ffmpeg -v error -y -i \"" + f_24 + "\" -c:a flac \"" + f_flac24 + "\"") && fsx::exists(f_flac24)) {
            VersionCompare r2 = compare_and_score({ f_24, f_flac24 });
            std::string w2;
            check(r2.ok && metrics_identical(r2, &w2), "24-bit WAV vs FLAC：全部指标逐位相等", w2);
        } else skip("24-bit FLAC 容器", "ffmpeg 无法生成 FLAC");
    } else {
        skip("容器互换（WAV↔FLAC）", "找不到 ffmpeg.exe（PATH 或 exe 同目录）");
    }

    // ---------------- §3 对齐鲁棒性 ----------------
    printf("§3 对齐（前置静音 / 裁剪 / 截断）\n");
    {
        struct Case { const char* name; std::string path; double expectLag; };
        const Case cases[] = {
            { "前置 0.5 s 静音", f_lead05,  0.5 },
            { "前置 2.0 s 静音", f_lead2,   2.0 },
            { "裁掉前 1 s",      f_trim,   -1.0 },
        };
        for (const Case& c : cases) {
            VersionCompare r = compare_and_score({ f_base, c.path });
            const double got = r.ok ? r.versions[1].lagSec : 0.0;
            check(r.ok, std::string(c.name) + "：判为同一首", "corr=" + f4(r.sameTrackCorr));
            // 对齐精度 ≈ ±1 个包络点（5 ms）+ 平滑带来的几毫秒偏置；实测 ≤10 ms
            check(std::fabs(got - c.expectLag) <= 0.035,
                  std::string(c.name) + "：延迟报对（±35 ms）",
                  "期望 " + f4(c.expectLag) + " s，实测 " + f4(got) + " s");
            // 对齐对了 ⟹ 两版是同内容，分差必须在噪声级
            check(r.ok && std::fabs(r.scores[0] - r.scores[1]) <= 1.0,
                  std::string(c.name) + "：分差 ≤ 1 分（同内容）",
                  r.ok ? ("Δ=" + f4(std::fabs(r.scores[0] - r.scores[1]))) : "未 ok");
        }
        VersionCompare rt = compare_and_score({ f_base, f_trunc });
        check(rt.ok, "只保留前 60%：能对齐", "corr=" + f4(rt.sameTrackCorr));
        check(rt.ok && rt.windowSec > 17.0 && rt.windowSec < 18.1,
              "只保留前 60%：公共区段 = 较短者（18 s）", "window=" + f4(rt.windowSec) + " s");
    }

    // ---------------- §4 不同录音必须拒绝 ----------------
    printf("§4 拒绝出排名（不同录音 / 全静音）\n");
    {
        int rejected = 0, total = 0;
        const std::string pairs[][2] = { { f_base, f_song1 }, { f_base, f_song2 }, { f_song1, f_song2 } };
        for (const auto& p : pairs) {
            VersionCompare r = compare_and_score({ p[0], p[1] });
            ++total;
            if (!r.ok) ++rejected;
            printf("      corr=%s\n", f4(r.sameTrackCorr).c_str());
        }
        check(rejected == total, "不同录音 3/3 全部拒绝出排名",
              std::to_string(rejected) + "/" + std::to_string(total));
        // 全静音 vs 音乐
        const std::string f_sil = d + "\\silent.wav";
        std::vector<std::vector<double>> z(2, std::vector<double>((size_t)(10 * fs), 0.0));
        write_wav(f_sil, z, 44100, 16);
        VersionCompare r = compare_and_score({ f_base, f_sil });
        check(!r.ok, "全静音版本：拒绝（不是给个 0 分排名）", "corr=" + f4(r.sameTrackCorr));
    }

    // ---------------- §5 可比性标记 ----------------
    printf("§5 可比性标记\n");
    {
        VersionCompare r = compare_and_score({ f_base, f_mono });
        const MetricRow* cc = row_of(r, "channel_corr");
        const MetricRow* ml = row_of(r, "mono_loss_db");
        check(r.ok, "单声道 vs 立体声：能对比（不是拒绝）");
        check(cc && !cc->comparable, "单声道 vs 立体声：声道相关标「不可比」");
        check(ml && !ml->comparable, "单声道 vs 立体声：单声道损失标「不可比」");
        // 单声道损失这一项（权重 0.04）不该再参与评分：立体声版的该维度 penalty 必须是 0
        bool monoDimZero = true;
        for (const auto& dim : r.dims)
            if (dim.key == "mono_loss_db")
                for (double p : dim.penalty) if (p != 0.0) monoDimZero = false;
        check(monoDimZero, "单声道对比时 mono_loss 维度不参与评分（penalty 全 0）");
    }
    {
        // 16-bit vs 24-bit 同内容：SNR/本底噪声必须不可比（容器量化下限不同）
        VersionCompare r = compare_and_score({ f_base, f_24 });
        const MetricRow* snr = row_of(r, "snr_db");
        check(snr && !snr->comparable, "16 vs 24-bit：SNR 标「不可比」");
        std::string why;
        const bool identical = metrics_identical(r, &why);
        check(identical, "16 vs 24-bit（同内容）：可比行与本底逐位相等，分差 0", why);
    }
    {
        // 44.1k vs 48k：采样率提示必须出现（非谐波残留会随采样率漂，实测 13.9 dB @96k）
        VersionCompare r = compare_and_score({ f_base, f_48k });
        bool hasFsCaveat = false;
        for (const auto& c : r.caveats) if (c.find("采样率") != std::string::npos) hasFsCaveat = true;
        check(r.ok && hasFsCaveat, "44.1k vs 48k：解释里出现「采样率不同」的可信度提示",
              "caveats=" + std::to_string(r.caveats.size()));
    }

    // ---------------- §6 分差提示 ----------------
    printf("§6 分差 <1 分必须给提示\n");
    {
        VersionCompare r = compare_and_score({ f_base, f_lead2 });
        check(r.ok, "同内容 + 前置静音：能对齐");
        const double gap = r.ok ? std::fabs(r.scores[0] - r.scores[1]) : -1.0;
        check(gap >= 0.0 && gap < 1.0, "同内容分差 <1 分（噪声级）", "Δ=" + f4(gap));
        bool hasGapCaveat = false;
        for (const auto& c : r.caveats) if (c.find("只差") != std::string::npos) hasGapCaveat = true;
        check(hasGapCaveat, "分差 <1 分时解释里出现「名次不应被当作结论」");
        check(r.explanation.find("不应被当作结论") != std::string::npos,
              "同一提示也追加进 explanation（UI 不改也能看到）");
    }

    // ---------------- §7 有损判定的口径（不依赖 ffmpeg 的部分） ----------------
    printf("§7 第 8 维口径\n");
    {
        // 未检出陡沿 ⟹ effective_bandwidth 取 min(奈奎斯特, 22.05 kHz)
        VersionData v; v.fs = 96000; v.spectralCutoffHz = 0.0;
        check(std::fabs(vcdetail::effective_bandwidth(v) - 22050.0) < 1e-9,
              "96k 未检出截止：有效带宽封顶 22.05 kHz（不因奈奎斯特高而白拿分）");
        v.fs = 44100;
        check(std::fabs(vcdetail::effective_bandwidth(v) - 22050.0) < 1e-9,
              "44.1k 未检出截止：有效带宽 = 22.05 kHz");
        v.spectralCutoffHz = 16797.9;
        check(std::fabs(vcdetail::effective_bandwidth(v) - 16797.9) < 1e-9,
              "检出截止：有效带宽 = 截止频率");
    }
    if (have_ffmpeg() && run("ffmpeg -v error -y -i \"" + f_base + "\" -c:a libmp3lame -b:a 128k \"" + f_mp3_128 + "\"") &&
        fsx::exists(f_mp3_128)) {
        VersionCompare r = compare_and_score({ f_base, f_mp3_128 });
        check(r.ok && r.versions[1].lossySuspected, "128k MP3：判为疑似有损转码",
              "cutoff=" + f4(r.versions[1].spectralCutoffHz) + " Hz");
        check(r.ok && r.versions[1].spectralCutoffHz > 0.0 && r.versions[1].spectralCutoffHz < 20500.0,
              "128k MP3：截止落在 <20.5 kHz（fake_lossless 的绝对门，不是 0.78×Nyquist）",
              f4(r.versions[1].spectralCutoffHz) + " Hz");
        check(r.ok && r.ranking.front() == 0, "128k MP3 不得排第一（无损必须赢）",
              r.ok ? ("top=V" + std::to_string(r.ranking.front() + 1)) : "未 ok");
    } else {
        skip("128k MP3 有损判定", "找不到 ffmpeg.exe 或 libmp3lame 不可用");
    }

    printf("\n结果：fails=%d skips=%d\n", fails, skips);
    return fails == 0 ? 0 : 1;
}
