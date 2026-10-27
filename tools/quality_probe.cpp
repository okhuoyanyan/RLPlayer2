// tools/quality_probe.cpp — 音质 19 项的置信度探针（只读文件分析，不开任何音频设备）
//
// 定位：本工具不是"跑一遍看看"，而是为「指标可不可信」提供可复现的数字：
//   * 走【生产解码路径】（PcmFile：WAV/DSF 分块流式，FLAC/MP3 整段解码），
//     与 rtaweb /api/health 看到的是同一份样本；
//   * 输出全部 19 项，double 按 %.17g 打印（位级可复现），另给一个 64 位指纹
//     fingerprint：把每个 double 的位模式按 FNV-1a 64 折叠 —— 两次运行指纹相同即逐位一致；
//   * --chunk 走 QualityAccumulator 分块（生产路径），--whole 走 measure_quality（参考路径），
//     两者必须逐位一致（quality.h 的硬约定），本工具把这条约定变成可执行的判据；
//   * --repeat N 重复 N 次，打印每次指纹与最大偏差，用来量化"重复性 σ"。
//
// 用法：
//   quality_probe <文件> [更多文件...] [--chunk N] [--whole] [--repeat N] [--maxsec S] [--csv]
//
// 纪律（铁律 1/2）：只读文件 + 只在 build-qc\media 下写？不写文件。不打开任何音频端点。
#include "analysis/version_compare.h"   // PcmFile（生产解码路径）
#include "analysis/quality.h"

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

using namespace rta::analysis;

namespace {

// 把 double 的位模式折进 FNV-1a 64（-0.0 与 0.0 位模式不同，正好用来抓符号零）
static uint64_t fnv_double(uint64_t h, double v) {
    uint64_t b = 0;
    std::memcpy(&b, &v, sizeof(b));
    for (int i = 0; i < 8; ++i) {
        h ^= (b >> (i * 8)) & 0xFFull;
        h *= 1099511628211ull;
    }
    return h;
}
static uint64_t fnv_u64(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFull;
        h *= 1099511628211ull;
    }
    return h;
}

struct Row {
    const char* key;
    double v;
};

static void print_json(const std::string& path, const QualityResult& r, double bits, double dur,
                       uint64_t fp, double ms, const char* mode) {
    std::printf("{\"file\":\"");
    for (char c : path) { if (c == '\\') std::printf("\\\\"); else if (c == '"') std::printf("\\\""); else std::printf("%c", c); }
    std::printf("\",\"mode\":\"%s\",\"fs\":%.17g,\"ch\":%d,\"bits\":%.17g,\"frames\":%llu,\"dur\":%.17g,"
                "\"fp\":\"%016llx\",\"ms\":%.17g",
                mode, r.fs, r.channels, bits, (unsigned long long)r.frames, dur,
                (unsigned long long)fp, ms);
    const Row rows[] = {
        {"peak_dbfs", r.peakDbfs}, {"rms_dbfs", r.rmsDbfs}, {"crest_db", r.crestDb},
        {"clipped_pct", r.clippedPct}, {"clipped_runs", (double)r.clippedRuns},
        {"clipped_samples", (double)r.clippedSamples}, {"longest_run", (double)r.longestClipRun},
        {"thdn_db", r.thdnDb}, {"thdn_pct", r.thdnPct}, {"thdn_avail", r.thdnAvailable ? 1.0 : 0.0},
        {"nontonal_db", r.nonTonalDb}, {"noise_floor", r.noiseFloorDbfs},
        {"spectral_min", r.spectralMinDbfs}, {"snr_db", r.snrDb},
        {"centroid_hz", r.centroidHz}, {"tilt_db_oct", r.tiltDbPerOct},
        {"rolloff95_hz", r.rolloff95Hz}, {"hf10k_db", r.hfRatio10kDb},
        {"corr", r.channelCorr}, {"mono_loss_db", r.monoLossDb}, {"side_mid_db", r.sideMidDb},
        {"dyn_p95p10_db", r.dynP95P10Db}, {"dyn_centroid_dbfs", r.dynCentroidDbfs},
        {"quiet_p10_dbfs", r.quietP10Dbfs}, {"silence_pct", r.silencePct},
        {"stereo", r.stereo ? 1.0 : 0.0}, {"valid", r.valid ? 1.0 : 0.0},
        {"spec_windows", (double)r.spectralWindows},
    };
    for (const Row& row : rows) std::printf(",\"%s\":%.17g", row.key, row.v);
    std::printf(",\"thdn_method\":\"%s\"", r.thdnMethod.c_str());
    if (r.stereo) std::printf(",\"imd_avail\":%d", r.imdAvailable ? 1 : 0);
    std::printf(",\"note\":\"");
    for (char c : r.note) { if (c == '\\') std::printf("\\\\"); else if (c == '"') std::printf("\\\""); else std::printf("%c", c); }
    std::printf("\"}\n");
}

static uint64_t fingerprint(const QualityResult& r) {
    uint64_t h = 1469598103934665603ull;
    const double vals[] = {
        r.peakDbfs, r.rmsDbfs, r.crestDb, r.clippedPct, (double)r.clippedSamples,
        (double)r.clippedRuns, (double)r.longestClipRun, r.thdnDb, r.thdnPct, r.nonTonalDb,
        r.noiseFloorDbfs, r.spectralMinDbfs, r.snrDb, r.centroidHz, r.tiltDbPerOct,
        r.rolloff95Hz, r.hfRatio10kDb, r.channelCorr, r.monoLossDb, r.sideMidDb,
        r.dynP95P10Db, r.dynCentroidDbfs, r.quietP10Dbfs, r.silencePct,
    };
    for (double v : vals) h = fnv_double(h, v);
    h = fnv_u64(h, (uint64_t)r.frames);
    h = fnv_u64(h, (uint64_t)r.spectralWindows);
    h = fnv_u64(h, r.thdnAvailable ? 1u : 0u);
    h = fnv_u64(h, r.spectralWindows ? 1u : 0u);
    return h;
}

// 一次完整分析：按 mode 选整段 / 分块；返回结果与耗时
static QualityResult analyze(PcmFile& src, size_t total, size_t chunk, bool whole,
                             QualityTimings* tmOut, bool* readOk) {
    const int chn = src.channels();
    const double fs = (double)src.rate();
    *readOk = true;
    if (whole) {
        // 整段解码到平面 double 再 measure_quality（参考路径；内存 O(曲长)）
        std::vector<double> planar((size_t)chn * total, 0.0);
        rta::audio::AudioBuffer buf;
        size_t got = 0;
        while (got < total) {
            const size_t n = std::min<size_t>(65536, total - got);
            const size_t g = src.read(got, n, buf);
            if (g == 0) break;
            for (size_t i = 0; i < g; ++i)
                for (int c = 0; c < chn; ++c)
                    planar[(size_t)c * total + (got + i)] = buf.sample(i, c);
            got += g;
            if (g < n) break;
        }
        if (got < total) *readOk = false;
        std::vector<const double*> p;
        for (int c = 0; c < chn; ++c) p.push_back(planar.data() + (size_t)c * total);
        const auto t0 = std::chrono::steady_clock::now();
        QualityResult r = measure_quality(p.data(), chn, got, fs);
        if (tmOut) {
            tmOut->timeDomainMs = 0.0;
            tmOut->spectralMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        }
        return r;
    }
    QualityAccumulator acc;
    acc.begin(total, chn, fs);
    rta::audio::AudioBuffer buf;
    std::vector<double> tmp;
    size_t pos = 0;
    while (pos < total) {
        const size_t n = std::min<size_t>(chunk ? chunk : 65536, total - pos);
        const size_t g = src.read(pos, n, buf);
        if (g == 0) break;
        tmp.resize(g * (size_t)chn);
        for (size_t i = 0; i < g; ++i)
            for (int c = 0; c < chn; ++c)
                tmp[i * (size_t)chn + (size_t)c] = buf.sample(i, c);
        acc.add_interleaved(tmp.data(), g);
        pos += g;
        if (g < n) break;
    }
    if (pos < total) *readOk = false;
    QualityTimings tm;
    QualityResult r = acc.finalize(&tm);
    if (tmOut) *tmOut = tm;
    return r;
}

} // namespace

// ★ 必须走宽字符命令行：main(int, char**) 拿到的是 ANSI 代码页（GBK）字节，
//   真实曲库路径里的中文/日文一律变成乱码，ffmpeg 会报 -42/EILSEQ（真因是路径不是 UTF-8）。
static std::string utf8_of(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static int run(const std::vector<std::string>& argv) {
    std::vector<std::string> files;
    size_t chunk = 65536;
    bool whole = false;
    int repeat = 1;
    double maxsec = 0.0;
    for (size_t i = 1; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (a == "--chunk" && i + 1 < argv.size()) { chunk = (size_t)std::strtoull(argv[++i].c_str(), nullptr, 10); }
        else if (a == "--whole") whole = true;
        else if (a == "--repeat" && i + 1 < argv.size()) { repeat = std::atoi(argv[++i].c_str()); }
        else if (a == "--maxsec" && i + 1 < argv.size()) { maxsec = std::atof(argv[++i].c_str()); }
        else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "未知选项 %s\n", a.c_str()); return 2; }
        else files.push_back(a);
    }
    if (files.empty()) {
        std::fprintf(stderr, "用法: quality_probe <文件...> [--chunk N] [--whole] [--repeat N] [--maxsec S]\n");
        return 2;
    }
    int bad = 0;
    for (const std::string& f : files) {
        PcmFile src;
        std::string why;
        if (!src.open(f, &why)) {
            std::fprintf(stderr, "打开失败 %s: %s\n", f.c_str(), why.c_str());
            ++bad;
            continue;
        }
        size_t total = src.totalFrames();
        if (maxsec > 0.0 && src.rate() > 0) {
            const size_t lim = (size_t)(maxsec * (double)src.rate());
            if (lim > 0 && lim < total) total = lim;
        }
        const double dur = (src.rate() > 0) ? (double)total / (double)src.rate() : 0.0;
        QualityResult first;
        uint64_t fp0 = 0;
        double ms0 = 0.0;
        bool allSame = true;
        double worst = 0.0;
        for (int rep = 0; rep < repeat; ++rep) {
            // 每次都重开文件：排除"同一 PcmFile 复用"的缓存效应
            PcmFile s2;
            if (!s2.open(f, &why)) { ++bad; break; }
            QualityTimings tm;
            bool ok = false;
            const auto t0 = std::chrono::steady_clock::now();
            QualityResult r = analyze(s2, total, chunk, whole, &tm, &ok);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const uint64_t fp = fingerprint(r);
            if (rep == 0) {
                first = r; fp0 = fp; ms0 = ms;
                print_json(f, r, (double)s2.bits(), dur, fp, ms, whole ? "whole" : "stream");
            } else {
                if (fp != fp0) {
                    allSame = false;
                    const double a[] = {first.peakDbfs, first.rmsDbfs, first.crestDb, first.clippedPct,
                                        first.thdnDb, first.nonTonalDb, first.noiseFloorDbfs,
                                        first.snrDb, first.centroidHz, first.tiltDbPerOct,
                                        first.rolloff95Hz, first.hfRatio10kDb, first.channelCorr,
                                        first.monoLossDb, first.sideMidDb, first.dynP95P10Db,
                                        first.dynCentroidDbfs, first.quietP10Dbfs, first.silencePct};
                    const double b[] = {r.peakDbfs, r.rmsDbfs, r.crestDb, r.clippedPct,
                                        r.thdnDb, r.nonTonalDb, r.noiseFloorDbfs,
                                        r.snrDb, r.centroidHz, r.tiltDbPerOct,
                                        r.rolloff95Hz, r.hfRatio10kDb, r.channelCorr,
                                        r.monoLossDb, r.sideMidDb, r.dynP95P10Db,
                                        r.dynCentroidDbfs, r.quietP10Dbfs, r.silencePct};
                    for (size_t k = 0; k < sizeof(a) / sizeof(a[0]); ++k)
                        worst = std::max(worst, std::fabs(a[k] - b[k]));
                }
            }
        }
        if (repeat > 1) {
            std::printf("{\"repeat\":%d,\"bitwise\":%s,\"worst_abs\":%.17g,\"file\":\"%s\"}\n",
                        repeat, allSame ? "true" : "false", worst, f.c_str());
            if (!allSame) ++bad;
        }
        (void)ms0;
    }
    return bad == 0 ? 0 : 1;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);     // 中文/日文 note 与 JSON 原样输出
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    args.reserve((size_t)std::max(argc, 1));
    for (int i = 0; i < argc; ++i) args.push_back(utf8_of(wargv ? wargv[i] : L""));
    if (wargv) LocalFree(wargv);
    if (args.empty()) args.push_back("quality_probe");
    return run(args);
}
