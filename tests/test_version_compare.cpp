// test_version_compare.cpp — 音频体检 B3：多版本对比（对齐 / 可比性 / 排序 / 解释）
// 判据：三个【已知差异】的人工版本 —— 原版 / 加失真 / 削波 0.5 dB —— 排序必须把原版排第一，
//       且每个版本的问题必须出现在正确的指标上（不是"跑通了"，是"测出来对不对"）。
// 全部临时文件、完全离线（不开设备）。
//
// ==================== 2026-10-27：第 8 维（有效带宽 / 有损转码）回归 ====================
// 背景：上一轮实测同一首 60 s 四版（FLAC / 320k / 128k / 低通 14k 假高解析）时，
//   排序给出 128k 第一、FLAC 最后，因为当时只有 7 个维度、没有带宽维；有损编码把峰值压低、
//   削波样本变少（276 段 → 51 段）⟹ 无损反而因"削波多"被扣分。
// 本轮新增第 8 维后，本文件加了三段回归（全部合成素材，不依赖 ffmpeg，判据可重复）：
//   §8  有损 vs 无损重排：无损必须第一、128k 必须最后或倒数、分差 ≥ 20 分，
//       并且**同时验证缺陷机制确实存在**（无损的削波段数更多、按旧口径会输）。
//   §9  无损 vs 无损：带宽维度全员同分（罚 0），DR/削波/失真这些维度仍要分出高下。
//   §10 极端用例：全静音 / 极短 / 单声道 —— 不崩、不误判（不误标"有损"）。
//   §11 指标表完整：原有 19 行一行不少，第 8 维是新增的第 20 行。
// 另：--report 模式把同一份评分打印成「截止 / 有损 / 各维度得分 / 总分」表，供实测取证。
#include "analysis/version_compare.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>

using namespace rta::analysis;
using rta::dsp::kPi;
namespace fsx = std::filesystem;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static std::string f2(double a, double b = 0.0) {
    char b2[128]; std::snprintf(b2, sizeof(b2), "%.4g / %.4g", a, b); return b2;
}

// ---- 手写 WAV（PCM 16/24-bit，立体声/单声道），16-bit 与 24-bit 各一条路径 ----
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
    const double scale = (bits == 16) ? 32768.0 : 8388608.0;
    const long long vmax = (bits == 16) ? 32767 : 8388607;
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

// 合成"音乐"：多音 + 慢包络 + 轻微噪声底。峰值归一化到 peak。
static std::vector<std::vector<double>> make_music(size_t n, double fs, double peak) {
    std::vector<std::vector<double>> ch(2, std::vector<double>(n, 0.0));
    unsigned long long s = 424242ull;
    auto uni = [&]() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                       return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0; };
    auto gauss = [&]() { const double u1 = std::max(uni(), 1e-12), u2 = uni();
                         return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2); };
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / fs;
        const double env = 0.45 + 0.55 * std::fabs(std::sin(2.0 * kPi * 0.21 * t));
        const double l = env * (0.34 * std::sin(2.0 * kPi * 196.0 * t)
                              + 0.22 * std::sin(2.0 * kPi * 294.0 * t)
                              + 0.12 * std::sin(2.0 * kPi * 1176.0 * t)
                              + 0.05 * std::sin(2.0 * kPi * 4410.0 * t));
        const double rr = env * (0.30 * std::sin(2.0 * kPi * 196.0 * t + 0.4)
                               + 0.20 * std::sin(2.0 * kPi * 392.0 * t)
                               + 0.10 * std::sin(2.0 * kPi * 1764.0 * t));
        ch[0][i] = l + 1e-4 * gauss();
        ch[1][i] = rr + 1e-4 * gauss();
    }
    double mx = 0.0;
    for (const auto& c : ch) for (double v : c) mx = std::max(mx, std::fabs(v));
    if (mx > 0.0) for (auto& c : ch) for (double& v : c) v *= peak / mx;
    return ch;
}

// ---- 宽带"音乐"（第 8 维专用）：在 make_music 上补到奈奎斯特的宽带内容 ----
// 为什么必须有它（否则测的是素材不是编码器）：截止检测的参考是「1–5 kHz 平均谱密度 −40 dB」。
// make_music 的合成素材在 5 kHz 以上本来就是空的，任何版本都会被判"有陡沿截止"。
// 真实录音在 20 kHz 附近仍有 ~−60 dBFS 的本底（话筒/空气/抖动），这里用 σ=1e-3 的白噪声
// （−60 dBFS）+ 几个高频乐音补上；实测这能让"无损版未检出截止"成立，而砖墙低通的版本
// 一定检出（见 §8 的打印）。
static std::vector<std::vector<double>> make_music_wide(size_t n, double fs, double peak) {
    auto ch = make_music(n, fs, peak);
    unsigned long long s = 99194853094755497ull;
    auto uni = [&]() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                       return (double)((s * 2685821657736338717ull) >> 11) / 9007199254740992.0; };
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / fs;
        const double hf = 0.020 * std::sin(2.0 * kPi * 11800.0 * t)
                        + 0.015 * std::sin(2.0 * kPi * 16400.0 * t)
                        + 0.012 * std::sin(2.0 * kPi * 19600.0 * t)
                        + 0.008 * std::sin(2.0 * kPi * 21500.0 * t);
        const double u1 = std::max(uni(), 1e-12), u2 = uni();
        const double noise = 1e-3 * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
        ch[0][i] += hf + noise;
        ch[1][i] += 0.9 * hf + 0.8 * noise;
    }
    double mx = 0.0;
    for (const auto& c : ch) for (double v : c) mx = std::max(mx, std::fabs(v));
    if (mx > 0.0) for (auto& c : ch) for (double& v : c) v *= peak / mx;
    return ch;
}

// 硬砖墙低通（频域置零）——模拟有损编码器的谱截止。为什么不用二阶 IIR：
// 2 极点巴特沃斯在截止处的斜率只有 ~12 dB/oct，fake_lossless 的「1/6 倍频程 >12 dB」
// 陡沿门判不过（实测 ffmpeg 串 8 级 lowpass 也只是勉强），那样测出来的是"没检出"，
// 会把回归判据变成假通过。编码器实际是频域量化的悬崖，这里直接照那个形状做。
static void brickwall_lowpass(std::vector<double>& x, double fs, double fc) {
    size_t N = 1; while (N < x.size()) N <<= 1;
    std::vector<rta::dsp::cplx> a(N, rta::dsp::cplx(0.0, 0.0));
    for (size_t i = 0; i < x.size(); ++i) a[i] = rta::dsp::cplx(x[i], 0.0);
    rta::dsp::fft(a, false);
    const double binHz = fs / (double)N;
    for (size_t k = 0; k <= N / 2; ++k) {
        if ((double)k * binHz <= fc) continue;
        a[k] = rta::dsp::cplx(0.0, 0.0);
        if (k > 0) a[N - k] = rta::dsp::cplx(0.0, 0.0);
    }
    rta::dsp::fft(a, true);
    for (size_t i = 0; i < x.size(); ++i) x[i] = a[i].real();
}
static void brickwall_all(std::vector<std::vector<double>>& ch, double fs, double fc) {
    for (auto& c : ch) brickwall_lowpass(c, fs, fc);
}

// 前置静音（模拟不同版本的前导静音不同）
static std::vector<std::vector<double>> with_lead(const std::vector<std::vector<double>>& ch,
                                                  size_t leadFrames, size_t tailFrames = 0) {
    std::vector<std::vector<double>> o(ch.size(), std::vector<double>(leadFrames + ch[0].size() + tailFrames, 0.0));
    for (size_t c = 0; c < ch.size(); ++c)
        for (size_t i = 0; i < ch[c].size(); ++i) o[c][leadFrames + i] = ch[c][i];
    return o;
}

// ---------------- --report：把评分打印成诊断表（实测取证用，不属于 ctest 判据） ----------------
// 用法: test_version_compare --report <a.wav> <b.mp3> ...
// 打印：每版的截止频率 / 有损判定 / 各维度得分 / 总分 / 名次，以及全部指标行。
// 路径含中文时请先把文件放到 ASCII 目录（char** argv 在 Windows 上是 ANSI 代码页）。
static std::string base_name(const std::string& p) {
    const size_t k = p.find_last_of("\\/");
    return (k == std::string::npos) ? p : p.substr(k + 1);
}
static int report_mode(const std::vector<std::string>& files) {
    CompareOptions opt;
    VersionCompare r = compare_versions(files, opt);
    if (!r.ok) { printf("对比失败: %s\n", r.error.c_str()); return 1; }
    score_versions(r);
    printf("公共区段 %.2f s，最小包络相关 %.4f\n\n", r.windowSec, r.sameTrackCorr);
    printf("%-3s %-26s %10s %6s %10s %8s %5s\n",
           "标签", "文件", "截止Hz", "有损", "带宽Hz", "总分", "名次");
    for (size_t i = 0; i < r.versions.size(); ++i) {
        const VersionData& v = r.versions[i];
        int rank = 0;
        for (size_t k = 0; k < r.ranking.size(); ++k) if (r.ranking[k] == i) rank = (int)k + 1;
        printf("%-3s %-26s %10.1f %6s %10.1f %8.2f %5d\n",
               v.label.c_str(), base_name(v.path).c_str(), v.spectralCutoffHz,
               v.lossySuspected ? "是" : "否",
               vcdetail::effective_bandwidth(v), r.scores[i], rank);
    }
    printf("\n各维度折算分（0..100；penalty=1 = 满罚）与权重：\n");
    printf("%-14s %6s", "维度", "权重");
    for (const auto& v : r.versions) printf(" %8s", v.label.c_str());
    printf("\n");
    for (const auto& d : r.dims) {
        printf("%-14s %6.2f", d.key.c_str(), d.weight);
        for (double s : d.score) printf(" %8.2f", s);
        printf("\n");
    }
    // 旧版口径（逐位复刻上一轮的 7 维原权重：dr .25 / clip .20 / snr .20 / crest .10 /
    // LRA .10 / 非谐波 .10 / 单声道 .05，没有带宽维、没有有损固定罚）——"重排前"的真值。
    // 现在与「现口径」并排打印，就是报告里的 before/after 表。
    {
        static const char* kOldKeys[] = { "dr_db", "snr_db", "clipped_pct", "crest_db", "lra", "non_tonal_db", "mono_loss_db" };
        static const double kOldW[] = { 0.25, 0.20, 0.20, 0.10, 0.10, 0.10, 0.05 };
        auto dim_pen = [&](const std::string& k, size_t i) {
            for (const auto& d : r.dims) if (d.key == k) return d.penalty[i];
            return 0.0;
        };
        auto row_ok = [&](const std::string& k) {
            for (const auto& row : r.rows) if (row.key == k) return row.comparable;
            return false;
        };
        std::vector<double> old(r.versions.size(), 0.0);
        for (size_t i = 0; i < r.versions.size(); ++i) {
            double pen = 0.0, ws = 0.0;
            for (size_t d = 0; d < 7; ++d) {
                if (!row_ok(kOldKeys[d])) continue;
                pen += kOldW[d] * dim_pen(kOldKeys[d], i);
                ws += kOldW[d];
            }
            old[i] = (ws > 0.0) ? 100.0 * (1.0 - pen / ws) : 0.0;
        }
        std::vector<size_t> ord(r.versions.size());
        for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
        std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return old[a] > old[b]; });
        printf("旧版口径（上一轮 7 维原权重，无带宽维）排序：");
        for (size_t k = 0; k < ord.size(); ++k)
            printf("%s%s(%.1f)", k ? " > " : "", r.versions[ord[k]].label.c_str(), old[ord[k]]);
        printf("\n");
    }
    // 反事实（去掉第 8 维与有损固定罚；其余权重沿用今天这份表）——看"这一维单独贡献了什么"。
    {
        double wsum = 0.0;
        for (const auto& d : r.dims) if (d.key != "cutoff_hz") wsum += d.weight;
        std::vector<double> legacy(r.versions.size(), 0.0);
        for (size_t i = 0; i < r.versions.size(); ++i) {
            double pen = 0.0;
            for (const auto& d : r.dims) {
                if (d.key == "cutoff_hz") continue;
                pen += d.weight * d.penalty[i];
            }
            legacy[i] = 100.0 * (1.0 - pen / wsum);
        }
        std::vector<size_t> order(r.versions.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return legacy[a] > legacy[b]; });
        printf("旧口径（无带宽维/无有损固定罚）排序：");
        for (size_t k = 0; k < order.size(); ++k)
            printf("%s%s(%.1f)", k ? " > " : "", r.versions[order[k]].label.c_str(), legacy[order[k]]);
        printf("\n\n");
    }
    printf("\n指标表（%zu 行）：\n", r.rows.size());
    for (const auto& row : r.rows) {
        printf("  %-30s %-5s", row.label.c_str(), row.unit.c_str());
        for (double x : row.values) printf(" %12.4g", x);
        if (!row.comparable) printf("  [不可比: %s]", row.why.c_str());
        printf("\n");
    }
    printf("\n%s\n", r.explanation.c_str());
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 2 && std::string(argv[1]) == "--report") {
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) files.push_back(argv[i]);
        if (files.size() < 2) { printf("用法: test_version_compare --report <a> <b> [...]\n"); return 2; }
        return report_mode(files);
    }
    const double fs = 48000.0;
    const size_t n = (size_t)(12.0 * fs);
    const double kTargetFs = fs;

    fsx::path dir = fsx::temp_directory_path() / "rta_qcompare";
    std::error_code ec;
    fsx::create_directories(dir, ec);

    // ---- 三个版本：原版 / 加失真（无削波）/ 削波 0.5 dB ----
    auto base = make_music(n, fs, 0.95);
    auto dist = base;
    // 三次非线性：三次谐波幅度 ≈ 0.75·k·A²；k=0.015、A≈0.95 ⟹ 约 1% THD（−40 dB）
    for (auto& c : dist) for (double& v : c) v = v + 0.015 * v * v * v;
    {
        double mx = 0.0;
        for (const auto& c : dist) for (double v : c) mx = std::max(mx, std::fabs(v));
        for (auto& c : dist) for (double& v : c) v *= 0.95 / mx;          // 归一到同峰值：不引入削波
    }
    auto clipv = base;
    const double g = std::pow(10.0, 0.5 / 20.0);                          // 抬高 0.5 dB
    for (auto& c : clipv) for (double& v : c) v = std::max(-1.0, std::min(1.0, v * g));

    const auto A = with_lead(base, (size_t)(0.5 * fs));                   // 前导 0.5 s
    const auto B = with_lead(dist, (size_t)(2.0 * fs));                   // 前导 2.0 s
    const auto C = with_lead(clipv, 0);                                   // 无前导

    const std::string pA = (dir / "v1_orig.wav").string();
    const std::string pB = (dir / "v2_thd.wav").string();
    const std::string pC = (dir / "v3_clip.wav").string();
    check(write_wav(pA, A, (uint32_t)kTargetFs, 24), "写 V1（原版 24-bit WAV）");
    check(write_wav(pB, B, (uint32_t)kTargetFs, 24), "写 V2（+失真 24-bit WAV）");
    check(write_wav(pC, C, (uint32_t)kTargetFs, 24), "写 V3（削波 0.5 dB 24-bit WAV）");

    printf("\n=== 1. 对齐：三个版本前导静音 0.5 s / 2.0 s / 0 s，时长不同 ===\n");
    CompareOptions opt;
    opt.verbose = true;
    VersionCompare r = compare_versions({ pA, pB, pC }, opt);
    check(r.ok, "对比成功（同曲判定通过）", r.error);
    if (r.ok) {
        printf("  窗口 %.2f s，最小包络相关 %.4f\n", r.windowSec, r.sameTrackCorr);
        check(r.sameTrackCorr > 0.95, "对齐后包络相关 > 0.95（同一内容平移）",
              f2(r.sameTrackCorr));
        // 延迟差应接近 ±1.5 s（2.0 − 0.5 = 1.5 s）
        const double d21 = r.versions[1].lagSec - r.versions[0].lagSec;
        check(std::fabs(d21 - 1.5) < 0.03, "V2−V1 的对齐延迟 = 1.50 s ±0.03 s",
              f2(d21));
        // 公共区段应≈原版剩余长度（12 s）— 多出来的前导被裁掉
        check(std::fabs(r.windowSec - 12.0) < 0.15, "公共区段 = 12.0 s（= 最短版本的内容全长）",
              f2(r.windowSec));
    }

    printf("\n=== 2. 逐项指标：问题必须落在正确的指标上 ===\n");
    if (r.ok) {
        score_versions(r);
        const auto& q1 = r.versions[0].quality;
        const auto& q2 = r.versions[1].quality;
        const auto& q3 = r.versions[2].quality;
        printf("  V1 原版：非谐波残留 %+.2f dB，削波 %.4f%%，DR %.2f dB\n",
               q1.nonTonalDb, q1.clippedPct, r.versions[0].dynamicsTimes.drDb);
        printf("  V2 加失真：非谐波残留 %+.2f dB，削波 %.4f%%\n", q2.nonTonalDb, q2.clippedPct);
        printf("  V3 削波 0.5 dB：非谐波残留 %+.2f dB，削波 %.4f%%（段数 %zu）\n",
               q3.nonTonalDb, q3.clippedPct, q3.clippedRuns);
        check(q2.nonTonalDb > q1.nonTonalDb + 1.0,
              "V2 的非谐波残留显著高于 V1（失真落到失真类指标上）",
              f2(q2.nonTonalDb, q1.nonTonalDb));
        check(q2.clippedPct < 0.001, "V2 未引入削波（失真与削波是两件事）",
              f2(q2.clippedPct));
        check(q3.clippedPct > 0.001, "V3 检出削波样本（占比非零）",
              f2(q3.clippedPct, (double)q3.clippedRuns));
        check(q3.clippedPct > q1.clippedPct, "V3 削波占比 > V1", f2(q3.clippedPct, q1.clippedPct));
        printf("  版本 | DR dB | LRA | crest dB | SNR dB | 非谐波 dB | 削波%% | 段 | 得分\n");
        for (size_t i = 0; i < r.versions.size(); ++i) {
            const VersionData& v = r.versions[i];
            printf("  %s | %.2f | %.2f | %.2f | %.2f | %.2f | %.4f | %zu | %.1f\n",
                   v.label.c_str(), v.dynamicsTimes.drDb, v.dynamicsTimes.lra, v.quality.crestDb,
                   v.quality.snrDb, v.quality.nonTonalDb, v.quality.clippedPct,
                   v.quality.clippedRuns, r.scores.empty() ? 0.0 : r.scores[i]);
        }
    }

    printf("\n=== 3. 综合排序：原版必须第一，削波版必须最后 ===\n");
    if (r.ok) {
        printf("  得分：V1=%.1f V2=%.1f V3=%.1f\n", r.scores[0], r.scores[1], r.scores[2]);
        printf("  排序：%s > %s > %s\n", r.versions[r.ranking[0]].label.c_str(),
               r.versions[r.ranking[1]].label.c_str(), r.versions[r.ranking[2]].label.c_str());
        check(r.ranking.size() == 3 && r.ranking[0] == 0, "V1（原版）排第一",
              f2(r.scores[0]));
        check(r.scores[0] > r.scores[1] && r.scores[0] > r.scores[2], "V1 得分高于两个受损版本",
              f2(r.scores[0], r.scores[1]));
        // V2（1% THD）与 V3（0.5 dB 削波：仅 0.02% 孤立样本、无连续段）谁更差由指标决定，
        // 不预设顺序；这里只要求排序与"谁的非谐波残留更差"一致。
        const bool v2Worse = r.versions[1].quality.nonTonalDb > r.versions[2].quality.nonTonalDb;
        const bool v2Last = (r.ranking[2] == 1);
        check(v2Worse == v2Last, "排序与失真指标一致（非谐波残留更差者排后）",
              f2(r.versions[1].quality.nonTonalDb, r.versions[2].quality.nonTonalDb));
    }

    printf("\n=== 3b. 重度削波（+6 dB）必须排最后：评分对连续削波敏感 ===\n");
    {
        auto heavy = base;
        const double g6 = std::pow(10.0, 6.0 / 20.0);
        for (auto& c : heavy) for (double& v : c) v = std::max(-1.0, std::min(1.0, v * g6));
        const std::string pH = (dir / "v4_heavyclip.wav").string();
        write_wav(pH, heavy, (uint32_t)fs, 24);
        VersionCompare rh = compare_versions({ pA, pH });
        if (!rh.ok) { check(false, "重削波对比应成立", rh.error); }
        else {
            score_versions(rh);
            printf("  削波占比 %.2f%%（段数 %zu），得分 V1=%.1f V4=%.1f\n",
                   rh.versions[1].quality.clippedPct, rh.versions[1].quality.clippedRuns,
                   rh.scores[0], rh.scores[1]);
            check(rh.versions[1].quality.clippedPct > 1.0, "重削波占比 > 1%",
                  f2(rh.versions[1].quality.clippedPct));
            check(rh.ranking[1] == 1, "重削波版本排最后", f2(rh.scores[1]));
        }
    }

    printf("\n=== 4. 解释文本：必须说清\"为什么 A 比 B 好\" ===\n");
    if (r.ok) {
        const bool hasWhy = r.explanation.find("为什么 V1 优于") != std::string::npos;
        const bool hasNum = r.explanation.find("动态范围") != std::string::npos
                         || r.explanation.find("削波样本占比") != std::string::npos;
        printf("%s", r.explanation.c_str());
        check(hasWhy, "解释里写明「为什么 V1 优于…」");
        check(hasNum, "解释里带具体指标名与数字（不是空话）");
    }

    printf("\n=== 5. 不可比项：位深不同必须标注且不参与评分 ===\n");
    {
        auto base2 = make_music((size_t)(8.0 * fs), fs, 0.9);
        const std::string p16 = (dir / "same_16bit.wav").string();
        const std::string p24 = (dir / "same_24bit.wav").string();
        write_wav(p16, base2, (uint32_t)fs, 16);
        write_wav(p24, base2, (uint32_t)fs, 24);
        VersionCompare rr = compare_versions({ p16, p24 });
        if (!rr.ok) {
            check(false, "16/24-bit 同一内容对比应成立", rr.error);
        } else {
            score_versions(rr);
            const MetricRow* snr = nullptr;
            const MetricRow* nf = nullptr;
            for (const auto& row : rr.rows) {
                if (row.key == "snr_db") snr = &row;
                if (row.key == "noise_floor_dbfs") nf = &row;
            }
            check(snr && !snr->comparable && snr->why.find("位深") != std::string::npos,
                  "位深不同 → SNR 标为不可比", snr ? snr->why : "缺失");
            check(nf && !nf->comparable, "位深不同 → 本底噪声标为不可比",
                  nf ? nf->why : "缺失");
            check(rr.explanation.find("不可比项") != std::string::npos,
                  "解释里列出不可比项");
            // 位深只差量化地板：DR/削波 这类不依赖位深的维度仍然可比
            const MetricRow* dr = nullptr;
            for (const auto& row : rr.rows) if (row.key == "dr_db") dr = &row;
            check(dr && dr->comparable, "位深不同不影响 DR 的可比性");
        }
    }

    printf("\n=== 6. 不同曲子：必须判为「不是同一首」并拒绝出排序 ===\n");
    {
        const std::string pD = (dir / "other_song.wav").string();
        std::vector<std::vector<double>> other(2, std::vector<double>(n, 0.0));
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / fs;
            other[0][i] = 0.4 * std::sin(2.0 * kPi * 523.25 * t) * (0.5 + 0.5 * std::sin(2.0 * kPi * 3.1 * t));
            other[1][i] = 0.4 * std::sin(2.0 * kPi * 659.25 * t) * (0.5 + 0.5 * std::sin(2.0 * kPi * 1.7 * t));
        }
        write_wav(pD, other, (uint32_t)fs, 24);
        VersionCompare rr = compare_versions({ pA, pD });
        printf("  包络相关 %.4f → sameTrack=%d，error=%s\n",
               rr.sameTrackCorr, (int)rr.sameTrack, rr.error.c_str());
        check(!rr.ok && !rr.sameTrack, "不同曲子 → 拒绝对比并给原因（不硬排）", rr.error);
    }

    printf("\n=== 7. 确定性：同一组文件跑两次，得分逐位相同 ===\n");
    if (r.ok) {
        VersionCompare r2 = compare_versions({ pA, pB, pC });
        score_versions(r2);
        bool same = (r2.scores.size() == r.scores.size());
        for (size_t i = 0; same && i < r.scores.size(); ++i) same = (r2.scores[i] == r.scores[i]);
        check(same, "两次运行得分逐位相同（==）");
        check(r2.explanation == r.explanation, "两次运行解释文本相同");
    }

    // ==================== 第 8 维回归（2026-10-27）====================
    printf("\n=== 8. 有损 vs 无损：无损必须第一，128k 必须最后或倒数（复现并修掉旧缺陷）===\n");
    {
        const double fsw = 48000.0;
        const size_t nw = (size_t)(10.0 * fsw);
        auto wide = make_music_wide(nw, fsw, 0.99);                    // 满带宽素材（到奈奎斯特有内容）
        // 无损版：+1.2 dB 后硬限幅 ⟹ 真削波（连续 ≥3 样本的平台）—— 这就是旧口径下"无损吃亏"的来源。
        // 为什么不用 +0.4 dB：那只是把个别峰值顶到 1.0，实测连续段数为 0，缺陷机制根本不在场，
        // 那样即使排序正确也证明不了"修的是这个问题"（铁律 3）。
        auto lossless = wide;
        const double gcl = std::pow(10.0, 1.2 / 20.0);
        for (auto& c : lossless) for (double& v : c) v = std::max(-1.0, std::min(1.0, v * gcl));
        // 有损版：对【同一母带】做砖墙低通（编码器谱截止），再降 0.5 dB（编码器峰值归一化 ⟹ 不再削波）
        auto lossy = [&](double fc) {
            auto v = lossless;
            brickwall_all(v, fsw, fc);
            for (auto& c : v) for (double& x : c) x *= 0.94;
            return v;
        };
        const auto L1 = with_lead(lossless, 0);
        const auto L2 = with_lead(lossy(20000.0), (size_t)(0.30 * fsw));
        const auto L3 = with_lead(lossy(16000.0), (size_t)(0.60 * fsw));
        const auto L4 = with_lead(lossy(14000.0), (size_t)(0.90 * fsw));
        const std::string q1 = (dir / "w1_lossless.wav").string();
        const std::string q2 = (dir / "w2_320k.wav").string();
        const std::string q3 = (dir / "w3_128k.wav").string();
        const std::string q4 = (dir / "w4_fake14k.wav").string();
        write_wav(q1, L1, (uint32_t)fsw, 24);
        write_wav(q2, L2, (uint32_t)fsw, 24);
        write_wav(q3, L3, (uint32_t)fsw, 24);
        write_wav(q4, L4, (uint32_t)fsw, 24);
        VersionCompare rw = compare_versions({ q1, q2, q3, q4 });
        if (!rw.ok) {
            check(false, "四版宽带对比应成立", rw.error);
        } else {
            score_versions(rw);
            printf("  %-3s %-18s %10s %6s %8s %9s %8s\n", "版", "文件", "截止kHz", "有损", "削波%", "削波段", "总分");
            for (size_t i = 0; i < rw.versions.size(); ++i) {
                const VersionData& v = rw.versions[i];
                printf("  %-3s %-18s %10.1f %6s %8.4f %9zu %8.2f\n", v.label.c_str(), base_name(v.path).c_str(),
                       v.spectralCutoffHz / 1000.0, v.lossySuspected ? "是" : "否",
                       v.quality.clippedPct, v.quality.clippedRuns, rw.scores[i]);
            }
            printf("  排序：");
            for (size_t k = 0; k < rw.ranking.size(); ++k)
                printf("%s%s(%.1f)", k ? " > " : "", rw.versions[rw.ranking[k]].label.c_str(),
                       rw.scores[rw.ranking[k]]);
            printf("\n");
            // ① 缺陷机制必须在场（否则这段测试证明不了"修的是那个问题"，铁律 3）
            check(rw.versions[0].quality.clippedRuns > rw.versions[2].quality.clippedRuns,
                  "缺陷机制在场：无损版的削波段数 > 128k 版（旧口径下无损因此吃亏）",
                  f2((double)rw.versions[0].quality.clippedRuns, (double)rw.versions[2].quality.clippedRuns));
            // ② 带宽/有损判定
            check(!rw.versions[0].lossySuspected, "无损版未被判为有损",
                  f2(rw.versions[0].spectralCutoffHz));
            check(rw.versions[2].lossySuspected && rw.versions[3].lossySuspected,
                  "128k 级(16 kHz)与低通 14k 版被判为有损");
            check(rw.versions[2].spectralCutoffHz > 14500.0 && rw.versions[2].spectralCutoffHz < 17000.0,
                  "128k 版截止落在 14.5~17.0 kHz", f2(rw.versions[2].spectralCutoffHz));
            check(rw.versions[3].spectralCutoffHz > 12500.0 && rw.versions[3].spectralCutoffHz < 15000.0,
                  "低通 14k 版截止落在 12.5~15.0 kHz", f2(rw.versions[3].spectralCutoffHz));
            check(rw.versions[1].spectralCutoffHz > 18500.0 && rw.versions[1].spectralCutoffHz < 21000.0,
                  "320k 版截止落在 18.5~21.0 kHz", f2(rw.versions[1].spectralCutoffHz));
            // ③ 排序
            check(rw.ranking[0] == 0, "无损版排第一", f2(rw.scores[0]));
            check(rw.ranking[3] == 2 || rw.ranking[3] == 3, "128k / 低通 14k 占最后一名");
            check(rw.ranking[2] == 2 || rw.ranking[2] == 3, "最后两名都是被有损处理过的版本");
            // 标定：带宽维 + 有损固定罚，单独让 128k 相对无损掉多少分 —— 必须 ≥20
            // （用户的硬要求：要显著扣分，并且压过"削波少"带来的加分；削波维最大摆幅只有 14 分）
            {
                double wsum = 0.0;
                for (const auto& d : rw.dims) wsum += d.weight;
                double pCut = 0.0;
                for (const auto& d : rw.dims) if (d.key == "cutoff_hz") pCut = d.penalty[2] - d.penalty[0];
                const double pts = 100.0 * (0.20 / wsum) * pCut
                                 + (rw.versions[2].lossySuspected ? vcdetail::kLossyPenaltyPoints : 0.0);
                printf("  第 8 维净扣分（V3 相对 V1）：带宽 %.1f 分 + 有损固定 %.1f 分 = %.1f 分\n",
                       100.0 * (0.20 / wsum) * pCut, vcdetail::kLossyPenaltyPoints, pts);
                check(pts >= 20.0, "带宽维 + 有损固定罚让 128k 净掉 ≥20 分（标定达标）", f2(pts));
                check(rw.scores[0] - rw.scores[2] >= 12.0,
                      "版面上无损仍领先 128k ≥12 分（扣掉无损自身的削波损失后）",
                      f2(rw.scores[0] - rw.scores[2]));
            }
            // ④ 反事实：把第 8 维与有损固定罚去掉（= 旧口径），排序会退回"128k 第一"
            {
                double wsum = 0.0;
                for (const auto& d : rw.dims) if (d.key != "cutoff_hz") wsum += d.weight;
                std::vector<double> legacy(rw.versions.size(), 0.0);
                for (size_t i = 0; i < rw.versions.size(); ++i) {
                    double pen = 0.0;
                    for (const auto& d : rw.dims) {
                        if (d.key == "cutoff_hz") continue;
                        pen += d.weight * d.penalty[i];
                    }
                    legacy[i] = 100.0 * (1.0 - pen / wsum);
                }
                size_t bestL = 0;
                for (size_t i = 1; i < legacy.size(); ++i) if (legacy[i] > legacy[bestL]) bestL = i;
                printf("  反事实（去掉第 8 维与有损固定罚）：V1=%.1f V2=%.1f V3=%.1f V4=%.1f ⟹ %s 第一\n",
                       legacy[0], legacy[1], legacy[2], legacy[3], rw.versions[bestL].label.c_str());
                check(bestL != 0, "去掉第 8 维后【有损版】反而第一（证明这一维就是翻转的原因）");
                check(rw.explanation.find("疑似有损转码") != std::string::npos,
                      "解释里逐版写明「疑似有损转码」与截止");
                check(rw.explanation.find("权重失效") == std::string::npos,
                      "解释里不出现「权重失效」自检警告（说明有损版没被判成第一）");
            }
        }
    }

    printf("\n=== 8b. 兜底：有损版合理胜出时，解释里必须出现红字自检警告 ===\n");
    {
        // 造一个「被毁掉的无损版」（+6 dB 硬削波：实测削波 11%、9030 段），
        // 它比 128k 版更差是应该的 —— 这时排序把有损版排第一是**正确**的，
        // 但 UI/解释必须明确警告"别看名次"（否则用户会以为 128k 比无损好）。
        const double fsw = 48000.0;
        const size_t nw = (size_t)(6.0 * fsw);
        auto wide = make_music_wide(nw, fsw, 0.95);
        auto ruined = wide;
        const double g6 = std::pow(10.0, 6.0 / 20.0);
        for (auto& c : ruined) for (double& v : c) v = std::max(-1.0, std::min(1.0, v * g6));
        auto cut = wide;
        brickwall_all(cut, fsw, 16000.0);
        for (auto& c : cut) for (double& x : c) x *= 0.94;
        const std::string r1 = (dir / "x1_ruined_lossless.wav").string();
        const std::string r2 = (dir / "x2_128k.wav").string();
        write_wav(r1, ruined, (uint32_t)fsw, 24);
        write_wav(r2, cut, (uint32_t)fsw, 24);
        VersionCompare rx = compare_versions({ r1, r2 });
        if (!rx.ok) {
            check(false, "毁掉的无损版 vs 128k 对比应成立", rx.error);
        } else {
            score_versions(rx);
            printf("  得分：毁掉的无损 %.1f vs 128k %.1f；第一名 = %s\n",
                   rx.scores[0], rx.scores[1], rx.versions[rx.ranking[0]].label.c_str());
            check(rx.versions[1].lossySuspected && !rx.versions[0].lossySuspected,
                  "128k 版被判有损、重削波的无损版未被判有损");
            check(rx.ranking[0] == 1, "有损版合理胜出（重削波的无损版确实更差）",
                  f2(rx.scores[0], rx.scores[1]));
            check(rx.explanation.find("权重失效") != std::string::npos,
                  "解释里出现「权重失效」红字警告（第一名是有损版时不能默认名次可信）");
        }
    }

    printf("\n=== 9. 无损 vs 无损：带宽维度不干扰，DR/削波/失真仍能分出高下 ===\n");
    {
        const double fsw = 48000.0;
        const size_t nw = (size_t)(8.0 * fsw);
        auto clean = make_music_wide(nw, fsw, 0.99);
        // 三次非线性 + 归一化：k=0.10 时三次项约 −40 dB（≈1% THD），明显高于 −60 dBFS 的宽带本底，
        // 才能让"失真落到非谐波残留上"这条判据有 3 dB 以上的余量（k=0.015 时只有 0.6 dB，测不出来）。
        auto thd = clean;
        for (auto& c : thd) for (double& v : c) v = v + 0.10 * v * v * v;
        {
            double mx = 0.0;
            for (const auto& c : thd) for (double v : c) mx = std::max(mx, std::fabs(v));
            for (auto& c : thd) for (double& v : c) v *= 0.99 / mx;
        }
        auto clipv = clean;
        const double g = std::pow(10.0, 0.5 / 20.0);
        for (auto& c : clipv) for (double& v : c) v = std::max(-1.0, std::min(1.0, v * g));
        const std::string s1 = (dir / "n1_clean.wav").string();
        const std::string s2 = (dir / "n2_thd.wav").string();
        const std::string s3 = (dir / "n3_clip.wav").string();
        write_wav(s1, with_lead(clean, 0), (uint32_t)fsw, 24);
        write_wav(s2, with_lead(thd, (size_t)(0.25 * fsw)), (uint32_t)fsw, 24);
        write_wav(s3, with_lead(clipv, (size_t)(0.50 * fsw)), (uint32_t)fsw, 24);
        VersionCompare rn = compare_versions({ s1, s2, s3 });
        if (!rn.ok) {
            check(false, "无损三版对比应成立", rn.error);
        } else {
            score_versions(rn);
            printf("  截止 %.0f / %.0f / %.0f Hz，有损标记 %d/%d/%d，得分 %.1f / %.1f / %.1f\n",
                   rn.versions[0].spectralCutoffHz, rn.versions[1].spectralCutoffHz, rn.versions[2].spectralCutoffHz,
                   (int)rn.versions[0].lossySuspected, (int)rn.versions[1].lossySuspected, (int)rn.versions[2].lossySuspected,
                   rn.scores[0], rn.scores[1], rn.scores[2]);
            check(!rn.versions[0].lossySuspected && !rn.versions[1].lossySuspected && !rn.versions[2].lossySuspected,
                  "三个无损版本都没被误判为有损");
            bool bwZero = true;
            for (const auto& d : rn.dims) if (d.key == "cutoff_hz") for (double p : d.penalty) bwZero = bwZero && (p <= 0.0);
            check(bwZero, "带宽维度对三个无损版均为 0 罚（不干扰无损之间的比较）");
            check(rn.ranking[0] == 0, "干净的无损版排第一", f2(rn.scores[0]));
            check(rn.scores[0] > rn.scores[1] && rn.scores[0] > rn.scores[2],
                  "干净版得分高于「加失真」与「轻微削波」两版", f2(rn.scores[1], rn.scores[2]));
            check(rn.versions[1].quality.nonTonalDb > rn.versions[0].quality.nonTonalDb + 3.0,
                  "失真落到非谐波残留上（问题落在正确指标）",
                  f2(rn.versions[1].quality.nonTonalDb, rn.versions[0].quality.nonTonalDb));
            check(rn.versions[2].quality.clippedPct > rn.versions[0].quality.clippedPct,
                  "削波落到削波占比上（问题落在正确指标）",
                  f2(rn.versions[2].quality.clippedPct, rn.versions[0].quality.clippedPct));
        }
    }

    printf("\n=== 10. 极端用例：全静音 / 极短 / 单声道（不崩、不误判）===\n");
    {
        const double fsw = 48000.0;
        // a) 全静音：必须明确拒绝，不许出排序、不许 NaN
        std::vector<std::vector<double>> sil(2, std::vector<double>(fs * 2, 0.0));
        const std::string z1 = (dir / "z1_silence.wav").string();
        const std::string z2 = (dir / "z2_silence.wav").string();
        write_wav(z1, sil, (uint32_t)fs, 16);
        write_wav(z2, sil, (uint32_t)fs, 16);
        VersionCompare rz = compare_versions({ z1, z2 });
        printf("  全静音: ok=%d error=%s\n", (int)rz.ok, rz.error.c_str());
        check(!rz.ok && !rz.error.empty(), "全静音 → 明确拒绝并给原因（不崩、不出排序）");
        check(rz.scores.empty(), "全静音 → 没有产生分数");
        // b) 极短（0.6 s，短于一个谱窗）：允许分析，但分数必须有限、不得误标有损
        auto tinyA = make_music_wide((size_t)(0.6 * fsw), fsw, 0.90);
        auto tinyB = tinyA;
        brickwall_all(tinyB, fsw, 16000.0);
        for (auto& c : tinyB) for (double& x : c) x *= 0.94;
        const std::string t1 = (dir / "t1_tiny.wav").string();
        const std::string t2 = (dir / "t2_tiny_cut.wav").string();
        write_wav(t1, tinyA, (uint32_t)fsw, 24);
        write_wav(t2, tinyB, (uint32_t)fsw, 24);
        VersionCompare rt = compare_versions({ t1, t2 });
        if (!rt.ok) {
            printf("  极短 0.6 s: 被拒绝（%s）\n", rt.error.c_str());
            check(!rt.error.empty(), "极短文件：要么明确拒绝并给原因，要么给出有限分数（此处拒绝）");
        } else {
            score_versions(rt);
            bool fin = true;
            for (double s : rt.scores) fin = fin && std::isfinite(s);
            printf("  极短 0.6 s: 窗口 %.2f s，得分 %.1f / %.1f，截止 %.0f / %.0f Hz，有损 %d/%d\n",
                   rt.windowSec, rt.scores[0], rt.scores[1], rt.versions[0].spectralCutoffHz,
                   rt.versions[1].spectralCutoffHz, (int)rt.versions[0].lossySuspected,
                   (int)rt.versions[1].lossySuspected);
            check(fin, "极短文件：分数有限（无 NaN/Inf）");
            check(!rt.versions[0].lossySuspected, "极短文件：满带宽版未被误判有损");
        }
        // c) 单声道：立体声指标不可比，但带宽/有损判定照常工作
        const size_t nm = (size_t)(6.0 * fsw);
        auto wideM = make_music_wide(nm, fsw, 0.90);
        std::vector<std::vector<double>> monoA(1, wideM[0]);
        std::vector<std::vector<double>> monoB(1, wideM[0]);
        brickwall_all(monoB, fsw, 16000.0);
        for (auto& c : monoB) for (double& x : c) x *= 0.94;
        const std::string m1 = (dir / "m1_mono.wav").string();
        const std::string m2 = (dir / "m2_mono_cut.wav").string();
        write_wav(m1, monoA, (uint32_t)fsw, 24);
        write_wav(m2, monoB, (uint32_t)fsw, 24);
        VersionCompare rm = compare_versions({ m1, m2 });
        if (!rm.ok) {
            check(false, "单声道两版对比应成立", rm.error);
        } else {
            score_versions(rm);
            bool fin = true;
            for (double s : rm.scores) fin = fin && std::isfinite(s);
            printf("  单声道: 得分 %.1f / %.1f，截止 %.0f / %.0f Hz，有损 %d/%d\n",
                   rm.scores[0], rm.scores[1], rm.versions[0].spectralCutoffHz,
                   rm.versions[1].spectralCutoffHz, (int)rm.versions[0].lossySuspected,
                   (int)rm.versions[1].lossySuspected);
            check(fin, "单声道：分数有限");
            const MetricRow* ch = nullptr;
            for (const auto& row : rm.rows) if (row.key == "channel_corr") ch = &row;
            check(ch && !ch->comparable, "单声道：声道相关标为不可比（不硬算）");
            check(!rm.versions[0].lossySuspected && rm.versions[1].lossySuspected,
                  "单声道：满带宽版不误判、低通版判为有损");
            check(rm.ranking[0] == 0, "单声道：满带宽版排第一", f2(rm.scores[0]));
        }
    }

    printf("\n=== 11. 指标表完整：原有 19 行一行不少 + 新增第 8 维 ===\n");
    if (r.ok) {
        static const char* kNeed[] = {
            "dr_db", "lra", "crest_db", "snr_db", "noise_floor_dbfs", "non_tonal_db", "thdn_db",
            "clipped_pct", "clipped_runs", "centroid_hz", "tilt_db_oct", "rolloff95_hz",
            "hf_ratio_10k_db", "channel_corr", "mono_loss_db", "dyn_p95_p10_db",
            "dyn_centroid_dbfs", "peak_dbfs", "lufs"
        };
        int missing = 0;
        for (const char* k : kNeed) {
            bool found = false;
            for (const auto& row : r.rows) if (row.key == k) found = true;
            if (!found) { ++missing; printf("  缺少指标行: %s\n", k); }
        }
        bool hasCut = false;
        for (const auto& row : r.rows) if (row.key == "cutoff_hz") hasCut = true;
        check(missing == 0, "原有 19 个指标行全部在位（加维度没有丢指标）",
              "缺 " + std::to_string(missing) + " 行");
        check(hasCut, "新增第 8 维指标行 cutoff_hz 在位");
        check(r.rows.size() == 20, "指标表共 20 行 = 19 原有 + 1 新增",
              std::to_string(r.rows.size()) + " 行");
        check(r.dims.size() == 8, "评分维度共 8 个（7 原有 + 有效带宽）",
              std::to_string(r.dims.size()) + " 个");
        bool wSum = false;
        for (const auto& d : r.dims) if (d.key == "cutoff_hz" && d.weight > 0.19 && d.weight < 0.21) wSum = true;
        check(wSum, "带宽维度权重 = 0.20（> 削波 0.14，能压过「削波少」的加分）");
    }

    fsx::remove_all(dir, ec);
    printf("\n%s（失败项 %d）\n", fails == 0 ? "全部通过" : "有失败项", fails);
    return fails == 0 ? 0 : 1;
}
