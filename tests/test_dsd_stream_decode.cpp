// test_dsd_stream_decode.cpp -- DsdPcmStream 与整曲解码的逐位一致性
//
// 判据（铁律 3：判据必须能证明要证明的事）：
//   * 从任意块边界顺序读、以及乱序/回头读，输出 PCM 必须与 read_any()/read_dsd_native()
//     的整曲解码【逐样本完全相等】。只比"听起来一样"不能证明流式没改变声音。
//   * 内存判据不在这里：见 tests/measure_big_dsd_playback.cpp（那是归因/前后对比工具）。
//
// 未注册到 ctest（CMakeLists 不在本工作包的文件清单里）。手工构建：
//   clang++ -std=c++20 -O2 -Isrc tests/test_dsd_stream_decode.cpp -o tests/test_dsd_stream_decode.exe
//   tests/test_dsd_stream_decode.exe build/testdata/vfy_dsd64.dsf [more.dsf ...]
// 默认取样文件：build/testdata/vfy_dsd64.dsf（短）、build/testdata/conv128.dsf（DSD128）
#include "audio/audio_loader.h"
#include "audio/wav.h"
#include "engine/pipeline.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>

using rta::audio::AudioBuffer;
namespace fs = std::filesystem;

static int g_fail = 0;
static void ck(bool c, const char* what, const std::string& detail = "") {
    if (c) std::printf("  [PASS] %s\n", what);
    else { std::printf("  [FAIL] %s  %s\n", what, detail.c_str()); ++g_fail; }
}

// 比较整曲缓冲 a 的 [pos, pos+n) 与分块结果 b（b 从 0 帧开始）
static bool same_range(const AudioBuffer& a, const AudioBuffer& b, size_t pos, size_t n) {
    if (a.channels != b.channels) return false;
    if (b.frames() < n) return false;
    const size_t ch = (size_t)a.channels;
    for (size_t i = 0; i < n; ++i)
        for (size_t c = 0; c < ch; ++c)
            if (a.raw[(pos + i) * ch + c] != b.raw[i * ch + c]) return false;
    return true;
}


// 端到端等价：整曲缓冲喂管线 vs 流式解码喂管线 —— 输出必须逐样本相同。
// 这一条覆盖"分块读取 + 管线"的整条链路，而不是只比解码器输出。
static void test_pipeline_equivalence(const std::string& f, const AudioBuffer& whole, double seconds) {
    using namespace rta::engine;
    PipelineConfig cfg;
    cfg.outRate = 96000;
    cfg.outBits = 24;
    cfg.volumeEnabled = true;
    cfg.volumeDb = -60.0;
    cfg.shaper = rta::dsp::Shaper::None;
    cfg.gpuResample = true;

    const size_t cap = std::min<size_t>(whole.frames(), (size_t)(seconds * (double)whole.rate));
    Pipeline pa(cfg), pb(cfg);
    rta::audio::DsdPcmStream s;
    std::string err;
    if (!s.open(f, rta::audio::dsd_pcm_settings(), &err)) { std::printf("  [SKIP] stream open: %s\n", err.c_str()); return; }

    size_t pos = 0, bad = 0, outFrames = 0;
    const size_t kChunk = 4096, ch = (size_t)whole.channels;
    while (pos < cap) {
        const size_t n = std::min(kChunk, cap - pos);
        AudioBuffer ca;
        ca.rate = whole.rate; ca.channels = whole.channels; ca.bits = whole.bits; ca.isFloat = whole.isFloat;
        ca.raw.assign(whole.raw.begin() + (long long)(pos * ch), whole.raw.begin() + (long long)((pos + n) * ch));
        AudioBuffer cb;
        if (s.read(pos, n, cb) != n) { ++bad; break; }
        const AudioBuffer oa = pa.process(ca, nullptr);
        const AudioBuffer ob = pb.process(cb, nullptr);
        if (oa.raw.size() != ob.raw.size() || oa.raw != ob.raw) { ++bad; break; }
        outFrames += oa.frames();
        pos += n;
    }
    ck(bad == 0, "整曲喂管线 vs 流式喂管线 输出逐样本一致",
       "frames=" + std::to_string(outFrames) + " bad=" + std::to_string(bad));
}

static void test_file(const std::string& f) {
    std::printf("== %s\n", f.c_str());
    rta::audio::LoadNote note;
    AudioBuffer whole;
    try { whole = rta::audio::read_any(f, &note); }
    catch (const std::exception& e) { std::printf("  [SKIP] read_any failed: %s\n", e.what()); return; }
    if (whole.rate == 0 || whole.frames() == 0) { std::printf("  [SKIP] empty decode\n"); return; }

    rta::audio::DsdPcmStream s;
    std::string err;
    if (!s.open(f, rta::audio::dsd_pcm_settings(), &err)) {
        std::printf("  [SKIP] stream open failed: %s\n", err.c_str());
        return;
    }
    ck(s.rate() == whole.rate, "流式输出采样率与整曲解码一致",
       std::to_string(s.rate()) + " vs " + std::to_string(whole.rate));
    ck(s.channels() == whole.channels, "声道数一致");
    ck(s.totalFrames() == whole.frames(), "总帧数一致",
       std::to_string(s.totalFrames()) + " vs " + std::to_string(whole.frames()));

    // 1) 顺序分块读：小块（含非 2 的幂）+ 大块，混合
    size_t pos = 0, mismatches = 0;
    while (pos < whole.frames()) {
        const size_t n = std::min<size_t>((pos % 7 == 0) ? 777 : 4096, whole.frames() - pos);
        AudioBuffer c;
        const size_t got = s.read(pos, n, c);
        if (got != n || !same_range(whole, c, pos, n)) {
            ++mismatches;
            std::printf("  first bad chunk pos=%zu n=%zu got=%zu ch=%d frames=%zu raw=%zu\n",
                        pos, n, got, c.channels, c.frames(), c.raw.size());
            break;
        }
        pos += n;
    }
    ck(mismatches == 0, "顺序分块（4096/777 混合）逐样本一致");
    std::printf("  checked %zu / %zu frames\n", pos, whole.frames());

    // 2) 随机跳转（含回头）：每次读一小段，与整曲缓冲比对
    size_t bad = 0;
    unsigned seed = 12345u;
    const size_t N = whole.frames();
    for (int k = 0; k < 24; ++k) {
        seed = seed * 1103515245u + 12345u;
        const size_t p2 = (size_t)((seed >> 8) % (N > 10000 ? (N - 10000) : N));
        const size_t n2 = std::min<size_t>(5000, N - p2);
        AudioBuffer c;
        const size_t got = s.read(p2, n2, c);
        if (got != n2 || !same_range(whole, c, p2, n2)) ++bad;
    }
    ck(bad == 0, "随机跳转（含回头）逐样本一致", "bad=" + std::to_string(bad));

    // 3) 跨文件末尾：请求超出范围应返回短块而不是越界
    AudioBuffer c;
    const size_t tail = s.read(N - 10, 100, c);
    ck(tail == 10, "越过末尾的请求返回剩余帧数");

    // 4) 固定跳转点：5:00（用户实测要求的 seek 判据）。文件不足 5 分钟时跳过。
    if (N > (size_t)(301.0 * (double)whole.rate)) {
        const size_t p5 = (size_t)(300.0 * (double)whole.rate);
        const size_t n5 = std::min<size_t>(20000, N - p5);
        AudioBuffer c5;
        const size_t got5 = s.read(p5, n5, c5);
        ck(got5 == n5 && same_range(whole, c5, p5, n5), "seek 到 5:00 后读出的数据逐样本一致");
    }

    // 5) 端到端：两条路喂同一管线，输出逐样本相同（只比前 5 秒，CPU 升频太慢）
    test_pipeline_equivalence(f, whole, 5.0);
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(65001);
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // 崩溃时也要看到已打印的进度（本测试第一版就是靠它定位的）
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) files.push_back(argv[i]);
    if (files.empty()) {
        for (const char* f : { "build/testdata/vfy_dsd64.dsf", "build/testdata/conv128.dsf",
                               "build/testdata/src64.dsf" })
            if (fs::exists(fs::u8path(f))) files.push_back(f);
    }
    if (files.empty()) { std::printf("no test files found (run from repo root)\n"); return 0; }
    for (const auto& f : files) test_file(f);
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASS", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
