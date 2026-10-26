// test_track_health.cpp — 音频体检 B1b：编排层（track_health / health_store）+ CLI 底层验证
// 全合成 + 临时文件，确定性可复现；与 B1a 的锚点（997 Hz / −23 dBFS 立体声 ⟹ −23.0 LUFS）
// 直接对齐，证明编排层没有改变底层口径。不碰音频设备、不读真实曲库。
#include "analysis/track_health.h"
#include "analysis/health_store.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace rta::analysis;
using rta::dsp::kPi;
namespace fsx = std::filesystem;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// 确定性构造：997 Hz / −23 dBFS 立体声 48k / 10 s。
// bits=16 走 16-bit 量化；shifted=true 时把同样的 16-bit 值 <<8 装进 24-bit 容器（fake24 用例）。
static rta::audio::AudioBuffer make_tone(int bits, bool shifted) {
    rta::audio::AudioBuffer ab;
    ab.rate = 48000;
    ab.channels = 2;
    ab.bits = bits;
    ab.isFloat = false;
    const size_t n = (size_t)(10.0 * 48000.0);
    ab.raw.resize(n * 2);
    const double amp = std::pow(10.0, -23.0 / 20.0);
    for (size_t i = 0; i < n; ++i) {
        const double x = amp * std::sin(2.0 * kPi * 997.0 * (double)i / 48000.0);
        const int32_t v16 = (int32_t)std::llround(x * 32768.0);
        for (int c = 0; c < 2; ++c) {
            const size_t k = i * 2 + (size_t)c;
            if (bits == 16)          ab.raw[k] = v16;
            else if (shifted)        ab.raw[k] = v16 << 8;
            else                     ab.raw[k] = (int32_t)std::llround(x * 8388608.0);
        }
    }
    return ab;
}

// 手写 44 字节头 + 16-bit PCM 数据（仅支持 16-bit 立体声，测试够用）
static bool write_wav44(const std::string& path, const rta::audio::AudioBuffer& ab) {
    if (ab.bits != 16 || ab.isFloat) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const uint32_t n = (uint32_t)ab.raw.size();
    const uint32_t dataBytes = n * 2;
    const uint32_t rate = ab.rate, ch = (uint32_t)ab.channels;
    const uint16_t blockAlign = (uint16_t)(ch * 2);
    const uint32_t byteRate = rate * (uint32_t)blockAlign;
    auto u32 = [&](uint32_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF));
                                 f.put((char)((v >> 16) & 0xFF)); f.put((char)((v >> 24) & 0xFF)); };
    auto u16 = [&](uint16_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF)); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16((uint16_t)ch); u32(rate); u32(byteRate); u16(blockAlign); u16(16);
    f.write("data", 4); u32(dataBytes);
    for (uint32_t i = 0; i < n; ++i) u16((uint16_t)(int16_t)ab.raw[i]);
    return (bool)f;
}


// ---------------- 流式体检的一致性判据 ----------------
static bool same_bits(double a, double b) {
    if (a == b) return true;                                  // 含 +0/-0
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}
// 逐位相同 = 全部指标 + 元信息 + 人话原因逐字相同（"差不多"不算）
static bool same_health(const TrackHealth& a, const TrackHealth& b) {
    return a.status == b.status && a.error == b.error
        && same_bits(a.drDb, b.drDb) && same_bits(a.lufs, b.lufs)
        && same_bits(a.lra, b.lra) && same_bits(a.peakDbfs, b.peakDbfs)
        && same_bits(a.cutoffHz, b.cutoffHz) && same_bits(a.confidence, b.confidence)
        && a.losslessSuspected == b.losslessSuspected && a.fake24bit == b.fake24bit
        && a.reason == b.reason
        && same_bits(a.durationSec, b.durationSec)
        && a.fs == b.fs && a.channels == b.channels && a.bits == b.bits;
}
static std::string health_brief(const TrackHealth& h) {
    char b[320];
    std::snprintf(b, sizeof(b),
                  "(DR %.17g / LUFS %.17g / LRA %.17g / peak %.17g / cutoff %.17g / conf %.17g / sus %d / fake24 %d / %s)",
                  h.drDb, h.lufs, h.lra, h.peakDbfs, h.cutoffHz, h.confidence,
                  (int)h.losslessSuspected, (int)h.fake24bit, h.status.c_str());
    return std::string(b);
}
static std::string max_diff_text(const TrackHealth& x, const TrackHealth& y) {
    auto d = [](double p, double q) { return std::fabs(p - q); };
    char buf[256];
    std::snprintf(buf, sizeof(buf), "max|Δ| = %.17g (dr %.3g, lufs %.3g, lra %.3g, peak %.3g, cutoff %.3g, conf %.3g)",
                  std::max({ d(x.drDb, y.drDb), d(x.lufs, y.lufs), d(x.lra, y.lra), d(x.peakDbfs, y.peakDbfs),
                             d(x.cutoffHz, y.cutoffHz), d(x.confidence, y.confidence) }),
                  d(x.drDb, y.drDb), d(x.lufs, y.lufs), d(x.lra, y.lra), d(x.peakDbfs, y.peakDbfs),
                  d(x.cutoffHz, y.cutoffHz), d(x.confidence, y.confidence));
    return std::string(buf);
}

// 通用 WAV 写出（8/16/24/32 整型 + 32-bit float）：raw 里存的是 AudioBuffer 的整数样本
static bool write_wav_any(const std::string& path, const rta::audio::AudioBuffer& ab) {
    const int bytesPer = ab.bits / 8;
    if (bytesPer < 1 || ab.channels <= 0) return false;
    const uint32_t dataBytes = (uint32_t)(ab.raw.size() * (size_t)bytesPer);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const uint16_t ch = (uint16_t)ab.channels;
    const uint16_t blockAlign = (uint16_t)(ch * bytesPer);
    const uint32_t byteRate = ab.rate * (uint32_t)blockAlign;
    auto u32 = [&](uint32_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF));
                                 f.put((char)((v >> 16) & 0xFF)); f.put((char)((v >> 24) & 0xFF)); };
    auto u16 = [&](uint16_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF)); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(ab.isFloat ? (uint16_t)3 : (uint16_t)1); u16(ch); u32(ab.rate);
    u32(byteRate); u16(blockAlign); u16((uint16_t)ab.bits);
    f.write("data", 4); u32(dataBytes);
    for (size_t i = 0; i < ab.raw.size(); ++i) {
        const int32_t v = ab.raw[i];
        if (ab.isFloat) {
            float x; std::memcpy(&x, &v, 4);
            uint32_t u; std::memcpy(&u, &x, 4);
            u32(u);
        } else if (ab.bits == 8) {
            f.put((char)(uint8_t)(v + 128));            // 8-bit WAV 无符号，中心 128
        } else {
            for (int k = 0; k < bytesPer; ++k) f.put((char)((v >> (8 * k)) & 0xFF));
        }
    }
    return (bool)f;
}

// 合成一个最小可用的 DSF（DSD64 单声道/立体声，1-bit 白噪位流）
static bool write_test_dsf(const std::string& path, int channels, uint32_t dsdRate, uint64_t bitsPerCh) {
    const uint32_t blockSize = 4096;
    const uint64_t needBytes = (bitsPerCh + 7) / 8;
    const uint64_t fullBlocks = needBytes / blockSize;
    const uint64_t rem = needBytes % blockSize;
    const uint64_t nBlocks = fullBlocks + (rem ? 1 : 0);
    const uint64_t dataSize = nBlocks * (uint64_t)channels * (uint64_t)blockSize;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    auto u32 = [&](uint32_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF));
                                 f.put((char)((v >> 16) & 0xFF)); f.put((char)((v >> 24) & 0xFF)); };
    auto u64 = [&](uint64_t v) { for (int k = 0; k < 8; ++k) f.put((char)((v >> (8 * k)) & 0xFF)); };
    const uint64_t totalSize = 28 + 52 + 12 + dataSize;
    f.write("DSD ", 4); u64(28); u64(totalSize); u64(0);
    f.write("fmt ", 4); u64(52);
    u32(1);                                   // format version
    u32(0);                                   // format id = DSD raw
    u32(2);                                   // channel type = stereo
    u32((uint32_t)channels);
    u32(dsdRate);
    u32(1);                                   // bits per sample
    u64(bitsPerCh);                           // sample count（每声道 1-bit 采样数）
    u32(blockSize);                           // block size per channel
    u32(0);                                   // reserved
    f.write("data", 4); u64(dataSize + 12);
    for (uint64_t b = 0; b < nBlocks; ++b) {
        for (int c = 0; c < channels; ++c) {
            uint32_t rng = 0x12345679u * (uint32_t)(b + 1) + 0x9E3779B9u * (uint32_t)(c + 1);
            for (uint32_t i = 0; i < blockSize; ++i) {
                rng = rng * 1664525u + 1013904223u;
                f.put((char)(uint8_t)(rng >> 24));
            }
        }
    }
    return (bool)f;
}

static std::string brief(const TrackHealth& h) {
    char b[256];
    std::snprintf(b, sizeof(b), "(LUFS %.3f, DR %.3f, LRA %.3f, peak %.3f, fake24 %d, %s) %s",
                  h.lufs, h.drDb, h.lra, h.peakDbfs, h.fake24bit ? 1 : 0, h.status.c_str(),
                  h.reason.c_str());
    return std::string(b);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string tmpDir = fsx::temp_directory_path().string();
    const std::string wavPath = tmpDir + "\\rtahealth_b1b_tone.wav";
    const std::string storePath = tmpDir + "\\rtahealth_b1b_store.json";

    printf("=== 1. analyze_buffer：997 Hz / −23 dBFS 立体声 48k / 10 s / 16-bit（B1a 锚点一致性）===\n");
    rta::audio::AudioBuffer tone16;
    TrackHealth h1;
    {
        tone16 = make_tone(16, false);
        const auto t0 = std::chrono::steady_clock::now();
        h1 = analyze_buffer(tone16);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("  %s（analyze_buffer 耗时 %.1f ms）\n", brief(h1).c_str(), ms);
        check(h1.status == "ready", "status = ready");
        check(std::fabs(h1.lufs - (-23.0)) <= 0.1, "lufs = −23.0 ±0.1（与 B1a 一致）", brief(h1));
        check(std::fabs(h1.drDb - 3.0) <= 0.5, "dr_db = 3.0 ±0.5（与 B1a 一致）", brief(h1));
        check(std::fabs(h1.peakDbfs - (-23.0)) <= 0.05, "peak_dbfs = −23.0 ±0.05", brief(h1));
        check(std::fabs(h1.durationSec - 10.0) <= 0.01, "duration_sec = 10 ±0.01");
        check(h1.fs == 48000 && h1.channels == 2 && h1.bits == 16, "元信息 fs/channels/bits");
        check(h1.analyzedAtMs > 1700000000000ll, "analyzed_at_ms 已填（Unix 毫秒量级）");
        check(!h1.fake24bit, "16-bit 素材 fake24 = false");
    }

    printf("\n=== 2. fake24：同内容 16-bit 值 <<8 装入 24-bit → true；真 24-bit 量化 → false ===\n");
    {
        TrackHealth hs = analyze_buffer(make_tone(24, true));
        printf("  shifted: %s\n", brief(hs).c_str());
        check(hs.status == "ready", "status = ready");
        check(hs.fake24bit, "fake24 = true（16-bit 内容 <<8 装入 24-bit）");
        check(std::fabs(hs.lufs - h1.lufs) <= 0.1, "<<8 装填不改变响度读数（±0.1）", brief(hs));
        TrackHealth ht = analyze_buffer(make_tone(24, false));
        printf("  true24 : %s\n", brief(ht).c_str());
        check(!ht.fake24bit, "真 24-bit 量化 fake24 = false（对照）");
    }

    printf("\n=== 3. 临时 WAV 端到端：analyze_file 与 analyze_buffer 数字一致（±0.05）===\n");
    {
        const bool written = write_wav44(wavPath, tone16);
        check(written, "临时 WAV 写出成功（44 字节头 + PCM）", wavPath);
        const auto t0 = std::chrono::steady_clock::now();
        const TrackHealth hf = analyze_file(wavPath);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("  file: %s（analyze_file 耗时 %.1f ms）\n", brief(hf).c_str(), ms);
        check(hf.status == "ready", "status = ready");
        check(std::fabs(hf.lufs - h1.lufs) <= 0.05, "lufs 与 analyze_buffer 一致（±0.05）",
              brief(hf) + " vs " + brief(h1));
        check(std::fabs(hf.drDb - h1.drDb) <= 0.05, "dr_db 一致（±0.05）");
        check(std::fabs(hf.lra - h1.lra) <= 0.05, "lra 一致（±0.05）");
        check(std::fabs(hf.peakDbfs - h1.peakDbfs) <= 0.05, "peak_dbfs 一致（±0.05）");
        check(hf.fs == 48000 && hf.channels == 2 && hf.bits == 16, "元信息一致（read_wav 回读）");
        std::error_code ec;
        fsx::remove(wavPath, ec);
        check(!fsx::exists(wavPath), "临时文件已删除");
    }

    printf("\n=== 4. analyze_file 不存在的路径 → status=error、error 非空、不抛异常 ===\n");
    {
        const std::string ghost = tmpDir + "\\rtahealth_b1b_no_such_file.wav";
        bool threw = false;
        TrackHealth hg;
        try {
            hg = analyze_file(ghost);
        } catch (...) { threw = true; }
        check(!threw, "未抛异常");
        check(hg.status == "error", "status = error", hg.error);
        check(!hg.error.empty(), "error 非空（人话原因）", hg.error);
        check(hg.analyzedAtMs > 0, "analyzed_at_ms 仍已填");
        const std::string j = track_health_json(hg);
        check(j.find("\"status\":\"error\"") != std::string::npos && j.find("\"error\":\"") != std::string::npos,
              "error 形态的 JSON 序列化正常");
    }

    printf("\n=== 5. HealthStore：落盘→重载命中；mtime/size 变化未命中；损坏文件报错不静默清空 ===\n");
    {
        // 5a upsert → save → 重新构造加载 → 命中（含中文 reason 往返 + 规范化路径命中）
        std::error_code ec;
        fsx::remove(storePath, ec);
        fsx::remove(storePath + ".tmp", ec);
        TrackHealth hCache = h1;
        hCache.reason = "测试往返：中文\"引号\"\\斜杠 与 16.2 kHz 截止";
        {
            HealthStore a(storePath);
            check(a.size() == 0 && a.loadError().empty(), "不存在的缓存文件 → 空库、无错误");
            a.upsert(wavPath, 111, 222, hCache);
            a.upsert(wavPath, 111, 222, hCache);          // 重复 upsert 覆盖同键
            check(a.save(), "save() 原子写成功");
            check(fsx::exists(storePath), "缓存文件已产生");
            check(!fsx::exists(storePath + ".tmp"), "tmp 已被 rename 消费");
        }
        TrackHealth out;
        HealthStore b(storePath);
        check(b.size() == 1 && b.loadError().empty(), "重新构造加载：1 条、无错误");
        check(b.lookup(wavPath, 111, 222, out), "同参命中");
        check(std::fabs(out.lufs - hCache.lufs) <= 1e-3 && std::fabs(out.drDb - hCache.drDb) <= 1e-3,
              "数值往返一致（%.3f 精度）", brief(out));
        check(out.reason == hCache.reason && out.status == "ready", "中文 reason / status 往返逐字一致", out.reason);
        check(b.lookup(tmpDir + "\\.\\rtahealth_b1b_tone.wav", 111, 222, out),
              "带 . 段的等价路径经 weakly_canonical 规范化后命中");
        check(!b.lookup(wavPath, 111, 223, out), "size 变化 → 未命中");
        check(!b.lookup(wavPath, 112, 222, out), "mtime 变化 → 未命中");

        // 5b 改 size 再写 → 旧条目被覆盖、新键命中
        {
            HealthStore c(storePath);
            c.upsert(wavPath, 111, 999, hCache);
            check(c.save(), "改 size 后 save 成功");
        }
        {
            HealthStore d(storePath);
            check(d.size() == 1, "同键覆盖（不是追加）");
            check(d.lookup(wavPath, 111, 999, out) && !d.lookup(wavPath, 111, 222, out),
                  "新三元组命中、旧三元组失效");
        }

        // 5c 手改坏缓存文件 → 重新加载报错不崩；save() 拒绝写盘；文件保持原样（不静默清空）
        const std::string garbage = "{{{ 这不是 JSON ！！";
        {
            std::ofstream g(storePath, std::ios::binary | std::ios::trunc);
            g << garbage;
        }
        HealthStore e(storePath);
        check(!e.loadError().empty(), "损坏文件 → loadError() 非空（如实报错）", e.loadError());
        check(!e.lookup(wavPath, 111, 999, out), "损坏库 lookup 一律未命中（不崩）");
        check(!e.save(), "损坏库 save() 拒绝写盘");
        std::ifstream back(storePath, std::ios::binary);
        const std::string after((std::istreambuf_iterator<char>(back)), std::istreambuf_iterator<char>());
        check(after == garbage, "损坏文件内容原样保留（没有被静默清空/覆盖）");
        fsx::remove(storePath, ec);
        fsx::remove(storePath + ".tmp", ec);
        fsx::remove(wavPath, ec);
    }

    printf("\n=== 6. track_health_json：字段名与契约逐字一致 + 转义回读 ===\n");
    {
        TrackHealth t;
        t.drDb = 8.125; t.lufs = -23.456; t.lra = 5.25; t.peakDbfs = -0.1;
        t.losslessSuspected = true; t.cutoffHz = 16193.0; t.confidence = 0.875;
        t.reason = "测试\"引号\"\\反斜杠\n换行\t制表 中文直通 16.2 kHz";
        t.fake24bit = true; t.durationSec = 123.4567; t.fs = 48000;
        t.channels = 2; t.bits = 24; t.status = "ready"; t.error = "";
        t.analyzedAtMs = 1761360000123ll;
        const std::string j = track_health_json(t);
        const char* keys[] = { "dr_db", "lufs", "lra", "peak_dbfs", "lossless_suspected", "cutoff_hz",
                               "confidence", "reason", "fake24", "duration_sec", "fs", "channels",
                               "bits", "status", "error", "analyzed_at_ms" };
        for (const char* k : keys) {
            const std::string needle = std::string("\"") + k + "\":";
            check(j.find(needle) != std::string::npos, std::string("字段名存在: ") + k);
        }
        const rta::net::Json p = rta::net::json_parse(j);
        check(p.ok(), "项目 JSON 解析器可解析（合法 JSON）");
        check(std::fabs(p["lufs"].n() - (-23.456)) < 5e-4, "lufs 数值往返", std::to_string(p["lufs"].n()));
        check(p["lossless_suspected"].b && p["fake24"].b, "布尔字段往返");
        check(p["reason"].s() == t.reason, "reason 转义回读逐字一致", p["reason"].s());
        check((int64_t)p["analyzed_at_ms"].n() == 1761360000123ll, "analyzed_at_ms 毫秒往返（double 精度内无损）");
        check((int)p["fs"].n() == 48000 && (int)p["channels"].n() == 2 && (int)p["bits"].n() == 24,
              "整数元信息往返");
    }


    printf("\n=== 7. 流式 vs 整段：WAV 各格式（8/16/24-bit + 32-bit float）× 块大小 1000/65536/262144 全逐位一致 ===\n");
    {
        const std::string dir = tmpDir + "\\rtahealth_stream_";
        // 文件名一律 ASCII：Windows 上 std::ofstream 的窄路径按 ANSI 代码页解释，中文名写不出来
        struct Case { const char* name; const char* file; int bits; bool isFloat; int ch; };
        const Case cases[] = {
            { "8-bit 单声道",       "wav8_mono.wav",     8,  false, 1 },
            { "16-bit 立体声",      "wav16_stereo.wav",  16, false, 2 },
            { "24-bit 立体声",      "wav24_stereo.wav",  24, false, 2 },
            { "32-bit float 立体声", "wav32f_stereo.wav", 32, true,  2 },
        };
        for (const Case& cs : cases) {
            rta::audio::AudioBuffer ab;
            ab.rate = 48000; ab.channels = cs.ch; ab.bits = cs.bits; ab.isFloat = cs.isFloat;
            const size_t n = (size_t)(6.0 * 48000.0);
            ab.raw.resize(n * (size_t)cs.ch);
            // 复合内容：997 Hz 正弦 + 慢包络 + 一段静音（同时压 DR / 门限 / 谱窗）
            for (size_t i = 0; i < n; ++i) {
                const double t = (double)i / 48000.0;
                double x = (0.5 + 0.4 * std::sin(2.0 * kPi * 0.3 * t))
                         * (0.25 * std::sin(2.0 * kPi * 997.0 * t) + 0.08 * std::sin(2.0 * kPi * 7300.0 * t));
                if (t > 4.0 && t < 4.5) x = 0.0;
                for (int c = 0; c < cs.ch; ++c) {
                    const double v = (c == 0) ? x : x * 0.7;
                    if (cs.isFloat) {
                        const float fv = (float)v;
                        int32_t bits32; std::memcpy(&bits32, &fv, 4);
                        ab.raw[i * (size_t)cs.ch + (size_t)c] = bits32;
                    } else {
                        ab.setSample(i, c, v);
                    }
                }
            }
            const std::string p = dir + cs.file;
            const bool ok = write_wav_any(p, ab);
            check(ok, std::string("写出 WAV: ") + cs.name, p);
            if (!ok) continue;
            const TrackHealth whole = analyze_file_whole(p);
            const TrackHealth s1 = analyze_file(p, [] { HealthStreamOptions o; o.blockFrames = 1000; return o; }());
            const TrackHealth s2 = analyze_file(p, [] { HealthStreamOptions o; o.blockFrames = 65536; return o; }());
            const TrackHealth s3 = analyze_file(p, [] { HealthStreamOptions o; o.blockFrames = 262144; return o; }());
            check(whole.status == "ready" && s2.status == "ready", std::string("status=ready: ") + cs.name, health_brief(whole));
            check(s2.streamed, std::string("确实走了流式分块（streamed=true）: ") + cs.name);
            check(same_health(whole, s1), std::string("整段 vs 流式(1000) 逐位一致: ") + cs.name, max_diff_text(whole, s1));
            check(same_health(whole, s2), std::string("整段 vs 流式(65536) 逐位一致: ") + cs.name, max_diff_text(whole, s2));
            check(same_health(whole, s3), std::string("整段 vs 流式(262144) 逐位一致: ") + cs.name, max_diff_text(whole, s3));
            printf("   %s %s\n", cs.name, health_brief(whole).c_str());
            std::error_code ec; fsx::remove(p, ec);
        }
    }

    printf("\n=== 8. 流式 vs 整段：合成 DSF（DSD64 → PCM 176.4k）逐位一致 ===\n");
    {
        const std::string p = tmpDir + "\\rtahealth_stream_test.dsf";
        const bool ok = write_test_dsf(p, 2, 2822400u, 2822400ull);   // 1 s / 立体声 / DSD64
        check(ok, "写出合成 DSF（DSD64, 1 s, 立体声）", p);
        if (ok) {
            const TrackHealth whole = analyze_file_whole(p);
            printf("   整段: %s\n   整段: %s\n", health_brief(whole).c_str(), whole.reason.c_str());
            check(whole.status == "ready", "整段解码 ready", whole.error);
            check(whole.fs == 176400 && whole.channels == 2 && whole.bits == 32,
                  "DSD64 → PCM 176.4 kHz / 2ch / 32bit", std::to_string(whole.fs));
            for (size_t B : {size_t(1000), size_t(65536), size_t(262144)}) {
                HealthStreamOptions o; o.blockFrames = B;
                const TrackHealth s = analyze_file(p, o);
                char b[128];
                std::snprintf(b, sizeof(b), "(块 %zu 帧, streamed=%d)", B, (int)s.streamed);
                check(s.streamed && same_health(whole, s),
                      std::string("DSD 整段 vs 流式逐位一致 ") + b, max_diff_text(whole, s));
            }
            std::error_code ec; fsx::remove(p, ec);
        }
    }

    printf("\n=== 9. 回退路径：WavPcmStream 拿不准的 WAV（data 在 fmt 之前）仍走整段且结果一致 ===\n");
    {
        // 手写一个 data 块在 fmt 之前的 WAV：read_wav 接受，parse_wav_header 拒绝 → 走整段回退
        const std::string p = tmpDir + "\\rtahealth_stream_datafirst.wav";
        const size_t n = (size_t)(2.0 * 48000.0);
        std::vector<int16_t> pcm(n * 2);
        for (size_t i = 0; i < n; ++i) {
            const double x = 0.2 * std::sin(2.0 * kPi * 997.0 * (double)i / 48000.0);
            pcm[i * 2] = (int16_t)std::llround(x * 32768.0);
            pcm[i * 2 + 1] = pcm[i * 2];
        }
        {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            auto u32 = [&](uint32_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF));
                                         f.put((char)((v >> 16) & 0xFF)); f.put((char)((v >> 24) & 0xFF)); };
            auto u16 = [&](uint16_t v) { f.put((char)(v & 0xFF)); f.put((char)((v >> 8) & 0xFF)); };
            const uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
            f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
            f.write("data", 4); u32(dataBytes);                        // data 在 fmt 之前
            for (int16_t v : pcm) u16((uint16_t)v);
            f.write("fmt ", 4); u32(16); u16(1); u16(2); u32(48000);
            u32(48000 * 4); u16(4); u16(16);
        }
        const TrackHealth whole = analyze_file_whole(p);
        HealthStreamOptions o; o.blockFrames = 65536;
        const TrackHealth s = analyze_file(p, o);
        check(whole.status == "ready", "整段路径接受该文件（read_wav 容忍 data 在 fmt 前）", whole.error);
        check(!s.streamed, "流式源拒绝 → 已回退整段（streamed=false）", health_brief(s));
        check(same_health(whole, s), "回退结果与整段逐位一致", max_diff_text(whole, s));
        std::error_code ec; fsx::remove(p, ec);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
