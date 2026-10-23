// dsdconv.cpp — DSD 域转换工具（ROADMAP P0-3 / P0-4 的命令行入口）
//
// 三个用途，都是"可判定的"：
//   1) --info            读 DSF/DFF 并如实报告容器/速率/声道/位序/时长
//   2) （默认）抽取       1-bit → PCM，FIR / IIR / CIC 三选一，写成 WAV（32-bit）
//   3) --direct           DirectSDM：1-bit → DoP 24-bit 载波 WAV，写出后再解包逐位比对
//
// 用法：
//   dsdconv --in x.dsf --info
//   dsdconv --in x.dsf [--kind fir|iir|cic] [--rate 176400] [--out pcm.wav]
//   dsdconv --in x.dsf --direct --out carrier.wav
// 说明：只读写文件，不打开任何音频设备（不发声）。
#include "audio/dsd_reader.h"
#include "audio/wav.h"
#include "dsp/direct_sdm.h"
#include "dsp/dsd_integrator.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace rta::dsp;
namespace audio = rta::audio;

static void usage() {
    printf("用法:\n"
           "  dsdconv --in x.dsf --info\n"
           "  dsdconv --in x.dsf [--kind fir|iir|cic] [--conv narrow|wide|xfi] [--rate 176400] [--out pcm.wav] [--two-stage|--single-stage]\n"
           "  dsdconv --in x.dsf --direct --out carrier.wav\n");
}

static IntegratorKind parse_kind(const std::string& s) {
    if (s == "iir") return IntegratorKind::Iir;
    if (s == "cic") return IntegratorKind::Cic;
    return IntegratorKind::Fir;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    std::string in, out, kindName = "fir", convName = "narrow";
    uint32_t rate = 0;
    bool info = false, direct = false, twoStage = true;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](const char* d) -> std::string { return (i + 1 < argc) ? argv[++i] : d; };
        if (k == "--in") in = next("");
        else if (k == "--out") out = next("");
        else if (k == "--kind") kindName = next("fir");
        else if (k == "--conv") convName = next("narrow");
        else if (k == "--rate") rate = (uint32_t)std::atoi(next("0").c_str());
        else if (k == "--info") info = true;
        else if (k == "--direct") direct = true;
        else if (k == "--single-stage" || k == "--no-2stage" || k == "--no-two-stage") twoStage = false;
        else if (k == "--two-stage" || k == "--2stage") twoStage = true;
        else if (k == "--dsd2stage") twoStage = (next("1") != "0");
        else if (k == "-h" || k == "--help") { usage(); return 0; }
    }
    if (in.empty()) { usage(); return 2; }

    audio::DsdStream s;
    try {
        s = audio::read_dsd(in);
    } catch (const std::exception& e) {
        printf("!! 读取失败：%s\n", e.what());
        return 1;
    }

    printf("输入: %s\n", in.c_str());
    printf("  容器 %s / %u Hz / %d ch / %llu 位每声道 / %.3f 秒 / 位序 %s\n",
           s.container.c_str(), s.rate, s.channels, (unsigned long long)s.bits,
           s.seconds(), audio::dsd_bit_order_name(s.fileOrder));
    if (!s.channelIds.empty()) {
        printf("  声道:");
        for (const auto& id : s.channelIds) printf(" [%s]", id.c_str());
        printf("\n");
    }
    const long long ones = s.ones();
    const long long total = (long long)s.bits * s.channels;
    printf("  位统计：%lld / %lld 为 1（%.2f%%）\n", ones, total, 100.0 * (double)ones / (double)total);

    if (info) return 0;

    if (direct) {
        if (out.empty()) { printf("--direct 需要 --out\n"); return 2; }
        std::vector<int32_t> packed;
        DopPackState st;
        const size_t frames = dsd_to_dop(s, packed, st);
        audio::AudioBuffer wav;
        wav.rate = s.rate / 16u;              // DoP 载波 = DSD/16
        wav.channels = s.channels;
        wav.bits = 24;
        wav.isFloat = false;
        wav.raw = packed;
        try { audio::write_wav(out, wav); } catch (const std::exception& e) {
            printf("!! 写出失败：%s\n", e.what()); return 1;
        }
        printf("  DirectSDM：%zu 载波帧/声道 @ %u Hz → %s\n", frames, wav.rate, out.c_str());

        // 解包回来逐位比对（只比 16 位对齐的部分；不足 16 位的尾巴按 DoP 规则丢弃）
        audio::DsdStream back;
        const bool ok = dop_to_dsd_bits(packed.data(), frames, s.channels, back);
        if (!ok) { printf("!! DoP 标记序列非法\n"); return 1; }
        const uint64_t cmpBits = (uint64_t)frames * 16;
        uint64_t diff = 0;
        for (int c = 0; c < s.channels; ++c)
            for (uint64_t n = 0; n < cmpBits; ++n)
                if (s.bit((size_t)c, n) != back.bit((size_t)c, n)) ++diff;
        printf("  逐位比对：%llu 位中不同 %llu 位%s\n", (unsigned long long)(cmpBits * (uint64_t)s.channels),
               (unsigned long long)diff, diff == 0 ? "  ✓ 逐位一致" : "  ✗ 不一致");
        printf("  DIRECT_SDM_BITEXACT=%d\n", diff == 0 ? 1 : 0);   // 给 verify.ps1 的机器可读判据
        if (s.bits % 16) printf("  [注] 源末尾 %llu 位不足一个 DoP 帧，按规范丢弃\n",
                                (unsigned long long)(s.bits % 16));
        return diff == 0 ? 0 : 1;
    }

    // ---- 默认：抽取成 PCM ----
    const uint32_t autoRate = std::min<uint32_t>(s.rate / 16u, 705600u);
    const uint32_t outRate = rate ? rate : autoRate;
    if (s.rate % outRate != 0) {
        printf("!! 输出速率 %u 不是源 DSD %u 的整数分频\n", outRate, s.rate);
        return 2;
    }
    const DsdConversion conv = parse_conversion(convName);
    IntegratorConfig cfg = conversion_integrator(parse_kind(kindName), conv, s.rate, outRate, 120.0, twoStage);
    DsdDecimator dec(cfg);
    auto planes = dec.process(s);
    printf("  抽取器 %s%s / Conversion %s：%u → %u Hz（1/%u），通带 %.1f kHz，阻带起点 %.1f kHz\n",
           dec.isTwoStage() ? "两级 " : "",
           integrator_name(cfg.kind), conversion_name(conv), s.rate, outRate, dec.decimation(),
           cfg.passbandHz / 1000.0, dec.stopbandStartHz() / 1000.0);
    if (cfg.kind == IntegratorKind::Fir) printf("  FIR 抽头 %zu，通带最差 %.4f dB，镜像处 %.1f dB\n",
           dec.firTaps().size(), [&] {
               double w = 0; for (int x = 0; x <= 400; ++x) w = std::max(w, std::fabs(dec.response_db(20000.0 * x / 400.0))); return w; }(),
           -dec.response_db((double)outRate - 20000.0));

    audio::AudioBuffer wav;
    wav.rate = outRate;
    wav.channels = s.channels;
    wav.bits = 32;
    wav.isFloat = false;
    const size_t n = planes.empty() ? 0 : planes[0].size();
    wav.raw.resize(n * (size_t)s.channels);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < s.channels; ++c)
            wav.setSample(i, c, planes[(size_t)c][i]);
    double peak = 0.0;
    for (int32_t v : wav.raw) peak = std::max(peak, std::fabs((double)v) / 2147483648.0);
    printf("  输出 %zu 帧（%.3f 秒），峰值 %.2f dBFS\n", n, (double)n / (double)outRate,
           20.0 * std::log10(peak + 1e-12));
    if (!out.empty()) {
        try { audio::write_wav(out, wav); } catch (const std::exception& e) {
            printf("!! 写出失败：%s\n", e.what()); return 1;
        }
        printf("  已写出 %s\n", out.c_str());
    }
    return 0;
}
