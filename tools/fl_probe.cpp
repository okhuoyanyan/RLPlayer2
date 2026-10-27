// fl_probe.cpp — 假无损（频谱截止）/ 动态（DR·LUFS·LRA·峰值）离线探针
//
// 用途：DEBUG 标定。只读 WAV（PCM 8/16/24/32 或 float32），不解码网络流、不开任何音频设备。
// 判据必须与产品跑同一份头文件（analysis/fake_lossless.h、analysis/dynamics.h），
// 所以这里只做「WAV → 平面 double → 调库 → 打印」，不复制任何算法。
//
// 用法：
//   fl_probe [--out x.tsv] [--trim-begin N] [--trim-end N] [--bands] file.wav [more.wav ...]
//     --trim-begin/--trim-end  帧数，用于「同一文件前后各裁 1 秒」的稳定性实验
//     --bands                  打印 8/10/12/14/16/18/20/22 kHz 八度带能量（相对 1–5 kHz），
//                              用来解释「为什么检出/为什么漏检」——判据是相对门，绝对电平无用
//
// 输出：stdout 一行一个文件（ASCII 数字）；--out 写 UTF-8 TSV（含中文 reason，供 read 工具看）
#include "analysis/fake_lossless.h"
#include "analysis/dynamics.h"
#include "audio/wav.h"
#include "dsp/fft.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

using namespace rta::analysis;

namespace {

std::string ascii_only(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) o.push_back(c < 0x80 ? (char)c : '?');
    return o;
}

const char* verdict_tag(int v) {
    switch (v) {
        case 1: return "lossy";
        case 2: return "bandlimit";
        case 3: return "unclear";
        default: return "none";
    }
}

std::string base_name(const std::string& p) {
    size_t i = p.find_last_of("\\/");
    return (i == std::string::npos) ? p : p.substr(i + 1);
}

// 单窗（中点，65536 点 Hann）八度带能量，相对 [1k..5k] 平均谱密度（dB）。
// 与 fake_lossless 的截止判据同一物理量：判据是「高于 ref−40 dB 的最高频段」。
struct BandReport {
    double at[8] = {0,0,0,0,0,0,0,0};   // 8/10/12/14/16/18/20/22 kHz 起点带宽 2 kHz 内能量
    bool ok = false;
};

BandReport band_report(const std::vector<double>& mono, double fs) {
    BandReport br;
    const size_t kW = 65536;
    if (mono.size() < kW || fs <= 0) return br;
    const size_t off = (mono.size() - kW) / 2;
    std::vector<rta::dsp::cplx> buf(kW);
    for (size_t i = 0; i < kW; ++i) {
        const double hann = 0.5 * (1.0 - std::cos(2.0 * rta::dsp::kPi * (double)i / (double)(kW - 1)));
        buf[i] = rta::dsp::cplx(mono[off + i] * hann, 0.0);
    }
    rta::dsp::fft(buf, false);
    const double binHz = fs / (double)kW;
    std::vector<double> mag(kW / 2 + 1);
    for (size_t k = 0; k <= kW / 2; ++k) mag[k] = std::abs(buf[k]);
    auto bandDb = [&](double lo, double hi) {
        double s = 0.0; size_t n = 0;
        for (size_t k = (size_t)std::floor(lo / binHz); k <= (size_t)std::ceil(hi / binHz) && k <= kW / 2; ++k) {
            s += mag[k] * mag[k]; ++n;
        }
        return 10.0 * std::log10(s / (double)std::max<size_t>(1, n) + 1e-300);
    };
    const double ref = bandDb(1000.0, 5000.0);
    const double start[8] = {8000,10000,12000,14000,16000,18000,20000,22000};
    for (int i = 0; i < 8; ++i) {
        if (start[i] + 2000.0 > fs * 0.5 + 1.0) { br.at[i] = -999.0; continue; }
        br.at[i] = bandDb(start[i], start[i] + 2000.0) - ref;
    }
    br.ok = true;
    return br;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> files;
    std::string outPath;
    long long trimBegin = 0, trimEnd = 0;
    int srcBitsOverride = -1;   // <0: 用 WAV 容器的位深；0: 跳过 fake24；>0: 指定原始位深
    bool bands = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (a == "--trim-begin" && i + 1 < argc) trimBegin = std::atoll(argv[++i]);
        else if (a == "--trim-end" && i + 1 < argc) trimEnd = std::atoll(argv[++i]);
        else if (a == "--srcbits" && i + 1 < argc) srcBitsOverride = std::atoi(argv[++i]);
        else if (a == "--bands") bands = true;
        else files.push_back(a);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: fl_probe [--out x.tsv] [--trim-begin N] [--trim-end N] [--bands] file.wav...\n");
        return 2;
    }

    FILE* of = nullptr;
    if (!outPath.empty()) {
        of = std::fopen(outPath.c_str(), "wb");
        if (!of) { std::fprintf(stderr, "cannot open %s\n", outPath.c_str()); return 2; }
        std::fprintf(of, "file\tframes\tfs\tch\tbits\tcutoff_hz\tconf\tedge_db\twindows\tfill\t"
                         "suspected\tfake24\tdr_db\tlufs\tlra\tpeak_dbfs\t"
                         "band8\tband10\tband12\tband14\tband16\tband18\tband20\t"
                         "verdict\tratio\tguard_bins\ttrue_peak_dbtp\treason\n");
    }

    int nbad = 0;
    for (const std::string& path : files) {
        rta::audio::AudioBuffer ab;
        try { ab = rta::audio::read_wav(path); }
        catch (const std::exception& e) {
            std::fprintf(stderr, "  read fail %s: %s\n", path.c_str(), e.what());
            ++nbad; continue;
        }
        const size_t total = ab.frames();
        const int nch = ab.channels;
        const double fs = (double)ab.rate;
        if (total == 0 || nch <= 0) { std::fprintf(stderr, "  empty %s\n", path.c_str()); ++nbad; continue; }
        size_t b0 = (size_t)std::max<long long>(0, trimBegin);
        if (b0 >= total) b0 = 0;
        size_t b1 = total;
        if (trimEnd > 0 && (size_t)trimEnd < total - b0) b1 = total - (size_t)trimEnd;
        const size_t n = b1 - b0;

        std::vector<std::vector<double>> ch((size_t)nch, std::vector<double>(n));
        std::vector<double> mono(n, 0.0);
        for (int c = 0; c < nch; ++c)
            for (size_t i = 0; i < n; ++i) {
                const double v = ab.sample(b0 + i, c);
                ch[(size_t)c][i] = v;
                mono[i] += v / (double)nch;
            }
        std::vector<const double*> p;
        for (auto& v : ch) p.push_back(v.data());

        // 位深：float 容器里的样本是“解码结果”，不代表原始容器位深。若调用方没给
        // --srcbits，就按“未知”处理（0）—— 否则 16-bit 内容解成 f32 后低 8 位恒零，
        // 会被 fake24bit 误判成“16-bit 装进 24-bit 容器”。
        const int bits = (srcBitsOverride >= 0) ? srcBitsOverride
                                                : (ab.isFloat ? 0 : ab.bits);
        const LosslessCheck lc = check_lossless(p.data(), nch, n, fs, bits);
        const DynamicsResult dr = measure_dynamics(p.data(), nch, n, fs);
        const BandReport br = bands ? band_report(mono, fs) : BandReport();

        std::printf("%-26s n=%-8zu fs=%-6.0f ch=%d bits=%-2d cut=%7.0f conf=%.2f edge=%5.1f "
                    "win=%2d fill=%.2f susp=%d v=%s r=%.3f gb=%-5d DR=%5.1f LUFS=%7.2f LRA=%5.1f "
                    "pk=%6.2f TP=%6.2f\n",
                    base_name(path).c_str(), n, fs, nch, bits, lc.cutoffHz, lc.confidence,
                    lc.edgeDb, lc.windows, lc.windowFill, lc.suspected ? 1 : 0,
                    verdict_tag(lc.fakeVerdict), lc.cutoffRatio, lc.guardBins,
                    dr.drDb, dr.lufsIntegrated, dr.lra, dr.peakDbfs, dr.truePeakDbtp);
        if (bands && br.ok)
            std::printf("      bands rel 1-5k: 8k=%.1f 10k=%.1f 12k=%.1f 14k=%.1f 16k=%.1f 18k=%.1f 20k=%.1f dB\n",
                        br.at[0], br.at[1], br.at[2], br.at[3], br.at[4], br.at[5], br.at[6]);
        if (!lc.suspected)
            std::printf("      reason(ascii): %s\n", ascii_only(lc.reason).c_str());

        if (of) {
            std::fprintf(of, "%s\t%zu\t%.0f\t%d\t%d\t%.1f\t%.3f\t%.1f\t%d\t%.2f\t%d\t%d\t"
                             "%.2f\t%.3f\t%.2f\t%.2f\t"
                             "%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%.4f\t%d\t%.2f\t%s\n",
                         base_name(path).c_str(), n, fs, nch, bits, lc.cutoffHz, lc.confidence,
                         lc.edgeDb, lc.windows, lc.windowFill,
                         lc.suspected ? 1 : 0, lc.fake24bit ? 1 : 0, dr.drDb, dr.lufsIntegrated,
                         dr.lra, dr.peakDbfs, br.at[0], br.at[1], br.at[2], br.at[3], br.at[4],
                         br.at[5], br.at[6], verdict_tag(lc.fakeVerdict), lc.cutoffRatio,
                         lc.guardBins, dr.truePeakDbtp, lc.reason.c_str());
        }
    }
    if (of) std::fclose(of);
    return nbad ? 1 : 0;
}
