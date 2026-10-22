// make_dsd.cpp — DSD 测试素材生成器（ROADMAP P0-2）
//
// 本机原本一个 DSD 文件都没有，DSD 通路的"闭环验证对象"只能自己造：
//   正弦 → 我们的 SDM 调制器（5 阶 / 1-bit）→ 打包成 DSF 或 DFF。
// 生成后立刻读回逐位比对；--verify 再抽取成 PCM，给出 THD+N 的明确数值。
//
// 用法：
//   make_dsd --out test.dsf [--rate 64|128|256] [--tone 1000] [--amp -6]
//            [--seconds 3] [--order 5] [--channels 2] [--kind dsf|dff]
//            [--block 4096] [--verify]
// 说明：--amp 是 dBFS，默认 -6；本工具只写文件，不打开任何音频设备（不发声）。
#include "audio/dsd_reader.h"
#include "dsp/dsd_integrator.h"
#include "dsp/fft.h"
#include "dsp/modulator.h"
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
using rta::audio::DsdStream;

static std::string fmt_size(uint64_t n) {
    char b[64];
    if (n >= (1ull << 20)) std::snprintf(b, sizeof(b), "%.2f MB", (double)n / 1048576.0);
    else std::snprintf(b, sizeof(b), "%.1f KB", (double)n / 1024.0);
    return b;
}

static DsdStream modulate(uint32_t rate, int channels, size_t nBits,
                          double toneHz, double amp, int order) {
    DsdStream s;
    s.rate = rate;
    s.channels = channels;
    s.bits = nBits;
    s.container = "mod";
    s.data.assign((size_t)channels, std::vector<uint8_t>((nBits + 7) / 8, 0));
    for (int c = 0; c < channels; ++c) {
        SdmConfig cfg;
        cfg.fs = (double)rate;
        cfg.order = order;
        cfg.cutoffHz = 100000.0;
        cfg.levels = 2;
        SdmModulator m(cfg);
        for (size_t n = 0; n < nBits; ++n) {
            const double x = amp * std::sin(2.0 * kPi * toneHz * (double)n / (double)rate);
            if (m.process(x) > 0.0) s.setBit((size_t)c, n, true);
        }
    }
    return s;
}

// 带限 THD+N（20 Hz…20 kHz）。
//
// 为什么不用"挖掉若干 bin"的 FFT 法：生成音是用户给的整数频率（1 kHz 等），
// 不一定落在 FFT 整 bin 上；Hann 窗之外的泄漏会落进噪声里，实测把 THD+N
// 从 98 dB 压到 33 dB —— 那是测量方法的错，不是 DSD 的错。
// 这里改成：先在时域用最小二乘把**基波**拟合掉（泄漏一并去掉），
// 再对残差加 Hann 窗做带限功率统计。谐波留在残差里，正是 THD+N 的定义。
static double thdn_db(const std::vector<double>& in, double fs, double f0, double& sigDb, double& noiseDb) {
    const size_t N = 65536;
    if (in.size() < N) return -999.0;
    const double w0 = 2.0 * kPi * f0 / fs;
    double cc = 0.0, ss = 0.0, cs = 0.0, xc = 0.0, xs = 0.0;
    for (size_t n = 0; n < N; ++n) {
        const double c = std::cos(w0 * (double)n), s = std::sin(w0 * (double)n);
        cc += c * c; ss += s * s; cs += c * s; xc += in[n] * c; xs += in[n] * s;
    }
    const double det = cc * ss - cs * cs;
    if (std::fabs(det) < 1e-300) return -999.0;
    const double A = (xc * ss - xs * cs) / det;
    const double B = (xs * cc - xc * cs) / det;

    std::vector<cplx> x(N);
    double sumW2 = 0.0;
    for (size_t n = 0; n < N; ++n) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * (double)n / (double)(N - 1));
        sumW2 += win * win;
        const double r = in[n] - (A * std::cos(w0 * (double)n) + B * std::sin(w0 * (double)n));
        x[n] = cplx(r * win, 0.0);
    }
    fft(x);
    const double binHz = fs / (double)N;
    const double norm = (double)N * sumW2;
    const size_t kLo = (size_t)std::ceil(20.0 / binHz);
    const size_t kTop = (size_t)std::floor(20000.0 / binHz);
    double pnoise = 0.0;
    for (size_t k = kLo; k <= kTop && k < N / 2; ++k) pnoise += std::norm(x[k]) / norm;
    const double psig = (A * A + B * B) / 2.0;
    sigDb = 10.0 * std::log10(psig + 1e-300);
    noiseDb = 10.0 * std::log10(2.0 * pnoise + 1e-300);
    return 10.0 * std::log10(psig / (2.0 * pnoise + 1e-300));
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    std::string out, kind = "dsf";
    int multiple = 64, channels = 2, order = 5;
    double tone = 1000.0, ampDb = -6.0, seconds = 3.0;
    uint32_t block = 4096;
    bool verify = false;

    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&](const char* def) -> std::string { return (i + 1 < argc) ? argv[++i] : def; };
        if (k == "--out") out = next("");
        else if (k == "--rate") multiple = std::atoi(next("64").c_str());
        else if (k == "--tone") tone = std::atof(next("1000").c_str());
        else if (k == "--amp") ampDb = std::atof(next("-6").c_str());
        else if (k == "--seconds") seconds = std::atof(next("3").c_str());
        else if (k == "--order") order = std::atoi(next("5").c_str());
        else if (k == "--channels") channels = std::atoi(next("2").c_str());
        else if (k == "--kind") kind = next("dsf");
        else if (k == "--block") block = (uint32_t)std::atoi(next("4096").c_str());
        else if (k == "--verify") verify = true;
        else if (k == "-h" || k == "--help") {
            printf("用法: make_dsd --out file.dsf [--rate 64|128|256] [--tone 1000] [--amp -6]\n"
                   "               [--seconds 3] [--order 5] [--channels 2] [--kind dsf|dff]\n"
                   "               [--block 4096] [--verify]\n");
            return 0;
        }
    }
    if (out.empty()) { printf("缺少 --out（用 -h 看用法）\n"); return 2; }
    if (multiple < 64) multiple = 64;
    if (channels < 1 || channels > 8) channels = 2;
    if (order < 1) order = 5;
    if (seconds <= 0.0 || seconds > 120.0) seconds = 3.0;

    const uint32_t rate = 44100u * (uint32_t)multiple;
    const size_t nBits = (size_t)std::llround(seconds * (double)rate);
    const double amp = std::pow(10.0, ampDb / 20.0);

    printf("make_dsd：DSD%d (%.4f MHz)，%d 声道，%.3f s，%.1f dBFS，%.1f Hz，%d 阶调制器\n",
           multiple, (double)rate / 1e6, channels, seconds, ampDb, tone, order);
    printf("  位数/声道 = %zu（%.2f MB 原始 1-bit 数据）\n", nBits,
           (double)nBits * (double)channels / 8.0 / 1048576.0);

    DsdStream s = modulate(rate, channels, nBits, tone, amp, order);
    for (int c = 0; c < channels; ++c) {
        s.channelIds = (channels == 2) ? std::vector<std::string>{ "SLFT", "SRGT" }
                                       : std::vector<std::string>{};
    }
    try {
        if (kind == "dff") rta::audio::write_dff(out, s);
        else               rta::audio::write_dsf(out, s, block);
    } catch (const std::exception& e) {
        printf("!! 写出失败：%s\n", e.what());
        return 1;
    }

    // 立刻读回：逐位比对（不信任自己的写出器）
    DsdStream back;
    uint64_t diffBits = 0;
    try {
        back = rta::audio::read_dsd(out);
        for (int c = 0; c < channels; ++c)
            for (uint64_t n = 0; n < nBits; ++n)
                if (s.bit((size_t)c, n) != back.bit((size_t)c, n)) ++diffBits;
    } catch (const std::exception& e) {
        printf("!! 读回失败：%s\n", e.what());
        return 1;
    }
    printf("  写出 %s，读回：%s %u Hz %d ch %llu 位，位序 %s\n",
           out.c_str(), back.container.c_str(), back.rate, back.channels,
           (unsigned long long)back.bits, rta::audio::dsd_bit_order_name(back.fileOrder));
    if (diffBits != 0) {
        printf("!! 读回的位流有 %llu 位不同（生成器/读取器不一致）\n", (unsigned long long)diffBits);
        return 1;
    }
    printf("  ✓ 读回逐位一致\n");

    if (verify) {
        // 抽取器：FIR（120 dB）→ THD+N
        IntegratorConfig cfg;
        cfg.kind = IntegratorKind::Fir;
        cfg.dsdRate = rate;
        cfg.outRate = rate / 16;                  // DSD64 -> 176.4k
        cfg.passbandHz = 20000.0;
        cfg.attenDb = 120.0;
        DsdDecimator dec(cfg);
        auto planes = dec.process(back);
        const double f0 = tone;      // 按实际生成频率测量（不是硬凑 FFT 整 bin）
        double sigDb = 0.0, noiseDb = 0.0;
        printf("  抽取：FIR %zu 抽头，%u Hz → %u Hz（%zu 帧）\n",
               dec.firTaps().size(), rate, cfg.outRate, planes.empty() ? 0 : planes[0].size());
        if (!planes.empty()) {
            // 丢掉 4096 个输出样本的启动瞬态，否则长滤波器/高 Q 的暂态会被算成失真
            const size_t skip = 4096;
            const std::vector<double> tail(planes[0].begin() + (long long)std::min(skip, planes[0].size()),
                                           planes[0].end());
            const double t = thdn_db(tail, (double)cfg.outRate, f0, sigDb, noiseDb);
            printf("  THD+N @ %.1f Hz = %.2f dB（信号 %.2f dB / 带内噪声 %.2f dB）\n",
                   f0, t, sigDb, noiseDb);
        }
    }

    // 文件大小
    FILE* f = std::fopen(out.c_str(), "rb");
    if (f) {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fclose(f);
        printf("  文件大小 %s\n", fmt_size((uint64_t)n).c_str());
    }
    return 0;
}
