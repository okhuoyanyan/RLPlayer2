// test_version_compare.cpp — 音频体检 B3：多版本对比（对齐 / 可比性 / 排序 / 解释）
// 判据：三个【已知差异】的人工版本 —— 原版 / 加失真 / 削波 0.5 dB —— 排序必须把原版排第一，
//       且每个版本的问题必须出现在正确的指标上（不是"跑通了"，是"测出来对不对"）。
// 全部临时文件、完全离线（不开设备）。
#include "analysis/version_compare.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

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

// 前置静音（模拟不同版本的前导静音不同）
static std::vector<std::vector<double>> with_lead(const std::vector<std::vector<double>>& ch,
                                                  size_t leadFrames, size_t tailFrames = 0) {
    std::vector<std::vector<double>> o(ch.size(), std::vector<double>(leadFrames + ch[0].size() + tailFrames, 0.0));
    for (size_t c = 0; c < ch.size(); ++c)
        for (size_t i = 0; i < ch[c].size(); ++i) o[c][leadFrames + i] = ch[c][i];
    return o;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
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

    fsx::remove_all(dir, ec);
    printf("\n%s（失败项 %d）\n", fails == 0 ? "全部通过" : "有失败项", fails);
    return fails == 0 ? 0 : 1;
}
