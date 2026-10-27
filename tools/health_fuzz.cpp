// health_fuzz.cpp — 分析链路 fuzz / 边界扫描工具（只读文件；不开任何音频设备；不联网）
//
// 为什么要有它（与 HTTP 层互补）：
//   /api/health 能证明「端到端可用」，但它把失败原因糊成一句话。要回答
//   「这次失败是路径编码、格式不支持、解码器，还是算法异常」，必须在**进程内**直接调
//   rta::analysis::analyze_file / analyze_file_whole，并分别拿到：
//     * HealthSource::open(why) 的拒绝原因（流式源判断）；
//     * TrackHealth.status / error（解码层与算法层）；
//     * QualityTimings（时域 vs 谱域耗时）。
//   本工具还把「同一文件重复 N 次是否逐位一致」「流式 vs 整段是否逐位一致」「8 线程并发
//   是否互不干扰」「连续 N 次后 RSS/句柄是否增长」做成可复现的子命令。
//
// 用法：
//   health_fuzz run        <listfile> [--whole] [--out <tsv>]   逐条分析（TSV）
//   health_fuzz stress     <listfile> <threads> [--out <tsv>]   并发分析（TSV，含结果哈希）
//   health_fuzz case       <path> [--repeat N]                  单文件详表 + 一致性
//   health_fuzz leak       <path> <N>                           连续 N 次，RSS/句柄曲线
//   health_fuzz rss                                            打印当前 RSS/句柄数
//
// 纪律：只读文件；不打开任何音频设备；不写文件系统（--out 例外）。
#include "analysis/track_health.h"
#include "analysis/health_stream.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string g_outPath;

size_t rss_bytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        return (size_t)pmc.WorkingSetSize;
    return 0;
}
unsigned handles() {
    DWORD n = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &n) ? (unsigned)n : 0;
}
double rss_mb() { return (double)rss_bytes() / (1024.0 * 1024.0); }

long long now_ms() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 结果指纹：把 analyzed_at_ms 抹成 0 再序列化（它是墙钟，天然每次不同），
// 其余 19 项指标必须逐位一致 —— 这是「确定性」的判据。
std::string stable_json(const rta::analysis::TrackHealth& h) {
    rta::analysis::TrackHealth c = h;
    c.analyzedAtMs = 0;
    return rta::analysis::track_health_json(c);
}
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
std::string hex16(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v);
    return b;
}
// TSV 字段里的制表/换行/回车必须压平，否则一行变多行
std::string flat(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
    return o;
}

std::vector<std::string> read_list(const char* p) {
    std::vector<std::string> v;
    std::ifstream f(p, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 1 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB)
            line.erase(0, 3);                    // 容忍 UTF-8 BOM
        if (!line.empty()) v.push_back(line);
    }
    return v;
}

// 单文件分析，返回 TSV 行（不含换行）。stream=true 走流式，false 走整段。
std::string analyze_row(const std::string& path, bool stream, bool wantWholeToo) {
    const long long t0 = now_ms();
    rta::analysis::TrackHealth h = stream
        ? rta::analysis::analyze_file(path, rta::analysis::HealthStreamOptions{})
        : rta::analysis::analyze_file_whole(path);
    const long long ms = now_ms() - t0;

    std::string extra;
    if (wantWholeToo && stream) {
        const long long w0 = now_ms();
        rta::analysis::TrackHealth w = rta::analysis::analyze_file_whole(path);
        const long long wms = now_ms() - w0;
        const bool same = (stable_json(h) == stable_json(w));
        extra = std::string("\t") + std::to_string(wms) + "\t" + (same ? "SAME" : "DIFF");
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%s\t%s\t%d\t%lld\t%.3f\t%u\t%d\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.1f\t%d\t%d\t%.1f\t%.1f\t%s",
                  path.c_str(),
                  h.status.empty() ? "?" : h.status.c_str(),
                  h.streamed ? 1 : 0, ms, h.durationSec, (unsigned)h.fs, h.channels, h.bits,
                  h.lufs, h.drDb, h.lra, h.peakDbfs, h.cutoffHz, h.fake24bit ? 1 : 0,
                  h.hasQuality ? 1 : 0,
                  h.qualityTimes.timeDomainMs, h.qualityTimes.spectralMs,
                  hex16(fnv1a(stable_json(h))).c_str());
    std::string row(buf);
    row += extra;
    // 非有限数检测（fuzz 判据）：JSON 规范里没有 inf/nan，一旦出现就是**客户端解析必崩**的响应。
    // 直接在序列化后的文本上找 token，比逐字段查更彻底（新加的字段自动被覆盖）。
    {
        const std::string js = stable_json(h);
        std::string bad;
        bool inStr = false, esc = false;
        for (size_t i = 0; i < js.size(); ++i) {
            const char c = js[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == 'n' && js.compare(i, 3, "nan") == 0) { bad += "nan "; i += 2; }
            else if (c == 'i' && js.compare(i, 3, "inf") == 0) { bad += "inf "; i += 2; }
            else if (c == '-' && (js.compare(i, 4, "-inf") == 0)) { bad += "-inf "; i += 3; }
        }
        row += "\t";
        row += bad.empty() ? "finite" : ("NONFINITE:" + flat(bad));
    }
    row += "\t";
    row += flat(h.error);
    return row;
}

void emit(const std::string& row) {
    static std::mutex mx;
    std::lock_guard<std::mutex> lk(mx);
    if (!g_outPath.empty()) {
        static std::ofstream fo(g_outPath, std::ios::binary | std::ios::trunc);
        fo << row << "\n";
        fo.flush();
    }
    std::printf("%s\n", row.c_str());
    std::fflush(stdout);
}

int cmd_run(const std::vector<std::string>& a) {
    if (a.size() < 2) { std::printf("run <listfile> [--whole] [--out f] [--both]\n"); return 2; }
    bool whole = false, both = false;
    for (size_t i = 2; i < a.size(); ++i) {
        if (a[i] == "--whole") whole = true;
        else if (a[i] == "--both") both = true;
        else if (a[i] == "--out" && i + 1 < a.size()) g_outPath = a[++i];
    }
    const auto files = read_list(a[1].c_str());
    std::printf("PATH\tSTATUS\tSTREAMED\tMS\tDUR\tFS\tCH\tBITS\tLUFS\tDR\tLRA\tPEAK\tCUTOFF\tFAKE24\tQVALID\tTD_MS\tSP_MS\tHASH%s\tNF\tERROR\n",
                both ? "\tWHOLE_MS\tCMP" : "");
    for (const auto& f : files) emit(analyze_row(f, !whole, both));
    std::printf("# rss_mb=%.1f handles=%u\n", rss_mb(), handles());
    return 0;
}

int cmd_stress(const std::vector<std::string>& a) {
    if (a.size() < 3) { std::printf("stress <listfile> <threads> [--out f]\n"); return 2; }
    const auto files = read_list(a[1].c_str());
    int th = std::atoi(a[2].c_str());
    if (th < 1) th = 1;
    for (size_t i = 3; i < a.size(); ++i)
        if (a[i] == "--out" && i + 1 < a.size()) g_outPath = a[++i];

    std::printf("PATH\tSTATUS\tSTREAMED\tMS\tDUR\tFS\tCH\tBITS\tLUFS\tDR\tLRA\tPEAK\tCUTOFF\tFAKE24\tQVALID\tTD_MS\tSP_MS\tHASH\tNF\tERROR\n");
    std::atomic<size_t> next{0};
    std::atomic<size_t> peakRss{0};
    const long long t0 = now_ms();
    std::vector<std::thread> pool;
    for (int t = 0; t < th; ++t) {
        pool.emplace_back([&]() {
            for (;;) {
                const size_t i = next.fetch_add(1);
                if (i >= files.size()) break;
                emit(analyze_row(files[i], true, false));
                const size_t r = rss_bytes();
                size_t prev = peakRss.load();
                while (r > prev && !peakRss.compare_exchange_weak(prev, r)) {}
            }
        });
    }
    for (auto& t : pool) t.join();
    std::printf("# threads=%d files=%zu wall_ms=%lld peak_rss_mb=%.1f end_rss_mb=%.1f handles=%u\n",
                th, files.size(), now_ms() - t0, (double)peakRss.load() / 1048576.0, rss_mb(), handles());
    return 0;
}

int cmd_case(const std::vector<std::string>& a) {
    if (a.size() < 2) { std::printf("case <path> [--repeat N]\n"); return 2; }
    const std::string path = a[1];
    int rep = 0;
    for (size_t i = 2; i < a.size(); ++i)
        if (a[i] == "--repeat" && i + 1 < a.size()) rep = std::atoi(a[++i].c_str());

    // ① 流式源能不能开、为什么不能开（把它与解码失败分开）
    {
        rta::analysis::detail::HealthSource src;
        std::string why;
        const bool ok = src.open(path, &why);
        std::printf("open=%d why=%s\n", ok ? 1 : 0, flat(why).c_str());
        if (ok) {
            std::printf("open_meta fs=%u ch=%d bits=%d float=%d frames=%zu kind=%d\n",
                        src.rate(), src.channels(), src.bits(), src.isFloat() ? 1 : 0,
                        src.totalFrames(), (int)src.kind());
        }
    }
    // ② 流式
    const long long t0 = now_ms();
    rta::analysis::TrackHealth hs = rta::analysis::analyze_file(path, rta::analysis::HealthStreamOptions{});
    const long long msS = now_ms() - t0;
    std::printf("stream ms=%lld status=%s streamed=%d err=%s\n", msS, hs.status.c_str(),
                hs.streamed ? 1 : 0, flat(hs.error).c_str());
    // ③ 整段
    const long long t1 = now_ms();
    rta::analysis::TrackHealth hw = rta::analysis::analyze_file_whole(path);
    const long long msW = now_ms() - t1;
    std::printf("whole  ms=%lld status=%s err=%s\n", msW, hw.status.c_str(), flat(hw.error).c_str());
    const bool same = (stable_json(hs) == stable_json(hw));
    std::printf("cmp stream_vs_whole=%s\n", same ? "SAME" : "DIFF");
    if (!same) {
        std::printf("  stream: %s\n", stable_json(hs).c_str());
        std::printf("  whole : %s\n", stable_json(hw).c_str());
    }
    std::printf("json=%s\n", stable_json(hs).c_str());
    // ④ 重复 N 次：逐位一致的判据
    if (rep > 0) {
        const std::string base = stable_json(hs);
        int diff = 0;
        for (int i = 0; i < rep; ++i) {
            rta::analysis::TrackHealth h = rta::analysis::analyze_file(path, rta::analysis::HealthStreamOptions{});
            if (stable_json(h) != base) { ++diff; std::printf("  rep%d DIFF: %s\n", i, stable_json(h).c_str()); }
        }
        std::printf("repeat n=%d diff=%d rss_mb=%.1f handles=%u\n", rep, diff, rss_mb(), handles());
    }
    return 0;
}

int cmd_leak(const std::vector<std::string>& a) {
    if (a.size() < 3) { std::printf("leak <path> <N>\n"); return 2; }
    const std::string path = a[1];
    const int n = std::atoi(a[2].c_str());
    std::printf("ITER\tRSS_MB\tHANDLES\tMS\tSTATUS\n");
    const double rss0 = rss_mb();
    const unsigned h0 = handles();
    long long total = 0;
    int fails = 0;
    for (int i = 1; i <= n; ++i) {
        const long long t0 = now_ms();
        rta::analysis::TrackHealth h = rta::analysis::analyze_file(path, rta::analysis::HealthStreamOptions{});
        const long long ms = now_ms() - t0;
        total += ms;
        if (h.status != "ready") ++fails;
        if (i % 10 == 0 || i == 1 || i == n)
            std::printf("%d\t%.1f\t%u\t%lld\t%s\n", i, rss_mb(), handles(), ms, h.status.c_str());
        std::fflush(stdout);
    }
    std::printf("# n=%d fails=%d avg_ms=%.1f rss_start_mb=%.1f rss_end_mb=%.1f rss_growth_mb=%.1f handles_start=%u handles_end=%u\n",
                n, fails, (double)total / (double)n, rss0, rss_mb(), rss_mb() - rss0, h0, handles());
    return 0;
}

int cmd_rss() {
    std::printf("rss_mb=%.1f handles=%u\n", rss_mb(), handles());
    return 0;
}

}  // namespace

// 命令行必须是宽字符：MS CRT 的窄 argv 按 ACP 转换，含中文的路径（本库 100%）会被
// 转成 GBK 字节，随即被当作 UTF-8 用 —— 传进去的路径必然打不开。这里用 wmain + 显式
// UTF-8 转换，避免「工具自己编码坏掉」被误判成「分析链路坏掉」。
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> a;
    for (int i = 1; i < argc; ++i) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(need > 0 ? (size_t)need - 1 : 0, '\0');
        if (need > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), need, nullptr, nullptr);
        a.push_back(s);
    }
    if (a.empty()) { std::printf("usage: run|stress|case|leak|rss\n"); return 2; }
    const std::string cmd = a[0];
    if (cmd == "run")    return cmd_run(a);
    if (cmd == "stress") return cmd_stress(a);
    if (cmd == "case")   return cmd_case(a);
    if (cmd == "leak")   return cmd_leak(a);
    if (cmd == "rss")    return cmd_rss();
    std::printf("unknown cmd: %s\n", cmd.c_str());
    return 2;
}
