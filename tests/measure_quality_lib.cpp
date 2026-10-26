// measure_quality_lib.cpp — 真实曲库上的音质/对比测量工具（不注册 ctest，手工构建）
//
// 用法：
//   measure_quality_lib.exe measure <文件...>        # 逐文件音质指标 + 耗时 + 内存
//   measure_quality_lib.exe compare <文件...> [--ai] # 多版本对比（--ai 追加 DeepSeek 解释）
//
// 纪律：只读分析，不开任何音频设备；AI 的 key 只从 KEY.txt/env/conf 读，绝不打印明文
//       （只打印来源与掩码）。
//
// 内存判据（铁律 3）：峰值工作集是【进程级单调量】。所以逐文件列的是"跑完该文件时的累计峰值"，
// 判据是「随着文件增多它不持续增长」；绝对增量只作参考。
#include "analysis/quality.h"
#include "analysis/version_compare.h"
#include "analysis/quality_ai.h"

#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rta::analysis;

static double peak_mb() {
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0.0;
    return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
}
static double ws_mb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) return 0.0;
    return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
}

static void dump_quality(const QualityResult& q) {
    std::printf("    THD+N        %8.2f dB   %s\n", q.thdnAvailable ? q.thdnDb : 0.0,
                q.thdnAvailable ? q.thdnMethod.c_str() : "不适用（宽带素材，无单一基频）");
    std::printf("    非谐波残留   %8.2f dB\n", q.nonTonalDb);
    std::printf("    本底噪声     %8.2f dBFS（100 ms 块 P10；谱最小统计 %8.2f dBFS 仅参考）\n",
                q.noiseFloorDbfs, q.spectralMinDbfs);
    std::printf("    信噪比       %8.2f dB（全轨 RMS − 本底）\n", q.snrDb);
    std::printf("    谱重心       %8.0f Hz\n", q.centroidHz);
    std::printf("    频谱倾斜     %8.2f dB/oct\n", q.tiltDbPerOct);
    std::printf("    95%%滚降      %8.0f Hz\n", q.rolloff95Hz);
    std::printf("    >10k 占比    %8.2f dB\n", q.hfRatio10kDb);
    std::printf("    峰值因子     %8.2f dB (peak %.2f dBFS)\n", q.crestDb, q.peakDbfs);
    std::printf("    削波         %zu 样本 / %.4f%% / %zu 段（最长 %zu）\n",
                q.clippedSamples, q.clippedPct, q.clippedRuns, q.longestClipRun);
    if (q.stereo)
        std::printf("    声道相关     %8.3f   单声道损失 %.2f dB   side/mid %.1f dB\n",
                    q.channelCorr, q.monoLossDb, q.sideMidDb);
    else
        std::printf("    声道相关     ——（%d 声道：非立体声，指标不适用）\n", q.channels);
    std::printf("    3s 动态 P95-P10 %5.2f dB   动态重心 %.2f dBFS   静音块 %.1f%%\n",
                q.dynP95P10Db, q.dynCentroidDbfs, q.silencePct);
}

static int cmd_measure(const std::vector<std::string>& files) {
    std::printf("MEASURE 起始 ws=%.1f MB peak=%.1f MB\n", ws_mb(), peak_mb());
    std::printf("%-58s %8s %8s %8s %10s %8s %8s\n",
                "文件", "时长s", "采集ms", "指标ms", "吞吐MB/s", "峰值WS", "增量WS");
    for (const std::string& f : files) {
        PcmFile src;
        std::string why;
        const double ws0 = ws_mb();
        if (!src.open(f, &why)) { std::printf("%-58s 打开失败: %s\n", f.c_str(), why.c_str()); continue; }
        const int chn = src.channels();
        const size_t total = src.totalFrames();
        const double fs = (double)src.rate();
        QualityAccumulator acc;
        acc.begin(total, chn, fs);
        rta::audio::AudioBuffer chunk;
        std::vector<double> tmp;
        size_t pos = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (pos < total) {
            const size_t n = std::min<size_t>(65536, total - pos);
            const size_t got = src.read(pos, n, chunk);
            if (got == 0) break;
            tmp.resize(got * (size_t)chn);
            for (size_t i = 0; i < got; ++i)
                for (int c = 0; c < chn; ++c) tmp[i * (size_t)chn + (size_t)c] = chunk.sample(i, c);
            acc.add_interleaved(tmp.data(), got);
            pos += got;
            if (got < n) break;
        }
        const double msCapture = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        QualityTimings tm;
        const auto t1 = std::chrono::steady_clock::now();
        const QualityResult q = acc.finalize(&tm);
        const double msMetrics = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        const double dur = (fs > 0) ? (double)total / fs : 0.0;
        const double audioMB = dur * fs / 1e6 * (double)chn * 8.0 / 1.0;  // 解码后 double 样本量
        const double through = (dur > 0) ? (audioMB / ((msCapture + msMetrics) / 1000.0)) : 0.0;
        char name[64];
        std::snprintf(name, sizeof(name), "%.56s", f.c_str());
        std::printf("%-58s %8.1f %8.0f %8.0f %10.1f %8.1f %8.1f\n", name, dur, msCapture, msMetrics,
                    through, peak_mb(), ws_mb() - ws0);
        dump_quality(q);
        std::printf("    [耗时分解] 逐样本采集 %.0f ms（%.0f%%）+ 时域指标 %.1f ms + 谱域(8×FFT及全部谱指标) %.1f ms；"
                    "谱窗缓冲 %.2f MB\n",
                    msCapture, 100.0 * msCapture / (msCapture + msMetrics + 1e-9),
                    tm.timeDomainMs, tm.spectralMs, (double)acc.windowBytes() / (1024.0 * 1024.0));
    }
    std::printf("MEASURE 结束 ws=%.1f MB peak=%.1f MB\n", ws_mb(), peak_mb());
    return 0;
}

static int cmd_compare(const std::vector<std::string>& files, bool useAi) {
    CompareOptions opt;
    opt.verbose = true;
    const VersionCompare r = compare_versions(files, opt);
    if (!r.ok) { std::printf("对比失败：%s\n", r.error.c_str()); return 1; }
    VersionCompare rr = r;
    score_versions(rr);
    std::printf("\n对齐：窗口 %.2f s，最小包络相关 %.4f，耗时 采集 %.0f ms / 对齐 %.0f ms / 指标 %.0f ms\n",
                rr.windowSec, rr.sameTrackCorr, rr.enrollMs, rr.alignMs, rr.analyzeMs);
    for (const VersionData& v : rr.versions)
        std::printf("  %s %s  %dch/%dbit/%uHz 偏移 %+.3f s 相关 %.4f\n",
                    v.label.c_str(), v.path.c_str(), v.channels, v.bits, v.fs, v.lagSec, v.envCorr);
    std::printf("\n== 逐项对比 ==\n");
    for (const MetricRow& row : rr.rows) {
        std::printf("  %-30s", row.label.c_str());
        if (!row.comparable) { std::printf(" 不可比：%s\n", row.why.c_str()); continue; }
        for (double v : row.values) std::printf(" %12.3f%s", v, row.unit.c_str());
        std::printf("%s\n", row.informational ? "  (信息性)" : "");
    }
    std::printf("\n== 得分 ==\n");
    for (size_t i = 0; i < rr.versions.size(); ++i)
        std::printf("  %s %.1f\n", rr.versions[i].label.c_str(), rr.scores[i]);
    std::printf("\n== 确定性解释 ==\n%s\n", rr.explanation.c_str());

    if (useAi) {
        AiExplainOptions aopt;
        AiExplain e = explain_versions(rr, aopt);
        std::printf("\n== AI 解释 ==\n");
        std::printf("  key 来源=%s model=%s tokens=%d ok=%d\n",
                    e.keySource.empty() ? "(无)" : e.keySource.c_str(), e.model.c_str(),
                    e.totalTokens, (int)e.ok);
        if (e.ok) std::printf("%s\n", e.text.c_str());
        else std::printf("  [降级] %s\n  → 使用确定性解释（见上）\n", e.error.c_str());
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(65001);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("usage: measure_quality_lib <measure|compare> <file...> [--ai]\n");
        return 2;
    }
    const std::string mode = rta::audio::wide_to_utf8(argv[1]);
    bool useAi = false;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        const std::string a = rta::audio::wide_to_utf8(argv[i]);
        if (a == "--ai") { useAi = true; continue; }
        files.push_back(a);
    }
    if (mode == "measure") return cmd_measure(files);
    if (mode == "compare") return cmd_compare(files, useAi);
    std::printf("未知模式 %s\n", mode.c_str());
    return 2;
}
