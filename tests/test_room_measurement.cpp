// test_room_measurement.cpp — 测量会话编排 C2：虚拟房间端到端（全部确定性、完全离线）
//
// 「虚拟房间」= 测试内实现 MeasurementTransport：播放序列与合成房间 IR 卷积、
// 加可控白噪/增益后作为「录音」返回。会话层看到的信号与真实 WASAPI 往返同构
// （同一参考序列 + 房间响应 + 噪声 + 增益设置），但全程不碰设备（铁律 1）。
//
// 判据纪律（铁律 3）：
//   * 延迟/极性/削波的真值全部来自构造参数（延迟样本数、反接开关、增益乘子），
//     不依赖被测代码反推真值；
//   * 闭环幅度判据与校正器设计用同一把尺子（同宽心理声学平滑，C1 坑 4），
//     阈值 2.0 dB = C1 合成判据 1.5 dB + 0.5 dB 测量路径余量（噪声底 + 正则化）；
//   * 噪声用固定种子 LCG 均匀白噪（乘 √3 使 RMS 等于标称值）：测试必须逐次可复现。
#include "dsp/room_measurement.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& w : v) { if (!s.empty()) s += " | "; s += w; }
    return s;
}
static std::string b2s(const std::vector<std::string>& v) {
    std::string s = "[";
    for (const auto& w : v) { if (s.size() > 1) s += ","; s += w; }
    return s + "]";
}
static bool contains(const std::vector<std::string>& v, const char* kw) {
    for (const auto& s : v) if (s.find(kw) != std::string::npos) return true;
    return false;
}

// FFT 线性卷积（与 C1 测试同款：全长 m+s−1，无时域卷绕）
static std::vector<double> fft_convolve(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.empty() || b.empty()) return {};
    const size_t need = a.size() + b.size() - 1;
    size_t n = 1;
    while (n < need) n <<= 1;
    std::vector<cplx> fa(n, cplx(0.0, 0.0)), fb(n, cplx(0.0, 0.0));
    for (size_t i = 0; i < a.size(); ++i) fa[i] = cplx(a[i], 0.0);
    for (size_t i = 0; i < b.size(); ++i) fb[i] = cplx(b[i], 0.0);
    fft(fa);
    fft(fb);
    for (size_t i = 0; i < n; ++i) fa[i] *= fb[i];
    fft(fa, true);
    std::vector<double> y(need);
    for (size_t i = 0; i < need; ++i) y[i] = fa[i].real();
    return y;
}

// 两个半谱幅度（dB）在 [fLo, fHi] 内的最大差
static double max_diff_db(const std::vector<double>& a, const std::vector<double>& b,
                          double fs, double fLo, double fHi, double* atHz = nullptr) {
    const double binHz = fs / (2.0 * (double)(a.size() - 1));
    double worst = -1e300, worstF = 0.0;
    for (size_t k = 0; k < a.size(); ++k) {
        const double f = (double)k * binHz;
        if (f < fLo || f > fHi) continue;
        const double d = std::fabs(a[k] - b[k]);
        if (d > worst) { worst = d; worstF = f; }
    }
    if (atHz) *atHz = worstF;
    return worst;
}

// 确定性均匀白噪（LCG，输出 [−1,1)）：不用随机设备——测试必须逐次可复现
struct Lcg {
    uint32_t s = 0xC2B0C2B0u;
    double next() { s = s * 1664525u + 1013904223u; return (double)(int32_t)s / 2147483648.0; }
};

// 房间 IR 配方（C1 §2 思路自实现）：解析幅度（100 Hz +4 dB 一阶低架 ×
// 8 kHz −3 dB 一阶高架 × 0.5 ms −6 dB 梳状）→ 最小相位 → 平移 delay。
// 峰值在 delay 处（最小相位 IR 能量集中于头部首样本）。
static double room_truth_db(double f) {
    const double k1 = std::pow(10.0, 4.0 / 10.0), r1 = f / 100.0;
    const double a2 = std::pow(10.0, -3.0 / 10.0), r2 = f / 8000.0;
    const double g = std::pow(10.0, -6.0 / 20.0);
    const double p1 = k1 * (1.0 + r1 * r1) / (1.0 + k1 * r1 * r1);
    const double p2 = (1.0 + a2 * r2 * r2) / (1.0 + r2 * r2);
    const double p3 = 1.0 + g * g + 2.0 * g * std::cos(2.0 * kPi * f * 0.0005);
    return 10.0 * std::log10(std::max(p1 * p2 * p3, 1e-12));
}
static std::vector<double> make_room_ir(double fs, size_t delaySamples) {
    const size_t bufferLen = (size_t)fs * 2;   // 2 s 缓冲，装下混响尾
    const size_t NR = 16384;
    std::vector<double> magLin(NR / 2 + 1);
    for (size_t k = 0; k <= NR / 2; ++k)
        magLin[k] = std::pow(10.0, room_truth_db((double)k * fs / NR) / 20.0);
    const auto hmp = minimum_phase_ir(magLin);
    std::vector<double> ir(bufferLen, 0.0);
    for (size_t i = 0; i < hmp.size() && i + delaySamples < bufferLen; ++i)
        ir[i + delaySamples] = hmp[i];
    return ir;
}
// 稀疏 δ 房间：超低对齐/极性判据用（延迟与增益的解析真值最干净）
static std::vector<double> delta_ir(double fs, size_t delaySamples, double gain = 1.0,
                                    bool invert = false) {
    std::vector<double> ir((size_t)fs * 2, 0.0);
    if (delaySamples < ir.size()) ir[delaySamples] = invert ? -gain : gain;
    return ir;
}

// 虚拟房间传输：play_and_capture = 卷积(播放序列, 声道房间 IR) + 可控白噪 × 可控增益。
// 声道按调用序号取 roomIrs（与会话的声道顺序天然对齐）；不做硬限幅——
// 真实链路里 |x|≥0.999 是 ADC 触顶前的预警区，会话层的削波质检正要在这之前触发。
struct VirtualRoomTransport : MeasurementTransport {
    std::vector<std::vector<double>> roomIrs;
    std::vector<double> channelNoiseDbfs;   // 每声道采集噪声（RMS dBFS）；空则用 noiseDbfs
    std::vector<double> channelExtraGain;   // 每声道输出乘子（>1 模拟增益过热）；空则 1.0
    double noiseDbfs = -80.0;
    int calls = 0;
    Lcg rng;
    std::vector<double> play_and_capture(const std::vector<double>& out, size_t n) override {
        const auto& ir = roomIrs[calls < (int)roomIrs.size() ? calls : (int)roomIrs.size() - 1];
        std::vector<double> y = fft_convolve(out, ir);
        y.resize(n, 0.0);
        const double rms = std::pow(10.0, (channelNoiseDbfs.size() >= roomIrs.size()
                                               ? channelNoiseDbfs[calls] : noiseDbfs) / 20.0);
        const double g = channelExtraGain.size() >= roomIrs.size() ? channelExtraGain[calls] : 1.0;
        for (auto& v : y) v = v * g + rms * 1.7320508 * rng.next();   // √3：均匀 [−1,1) 的 RMS 归一
        ++calls;
        return y;
    }
};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const double fs = 48000.0;
    SweepSpec spec;                     // 频带默认 10 Hz~22050 Hz；时长缩到 4 s：
    spec.durSec = 4.0;                  // 采集 5 s、反卷积 2^19 点，判据不变、测试提速
    const auto layouts = room_layouts();
    const RoomLayout* lay20 = nullptr;
    const RoomLayout* lay21 = nullptr;
    for (const auto& r : layouts) {
        if (std::string(r.name) == "2.0") lay20 = &r;
        if (std::string(r.name) == "2.1") lay21 = &r;
    }
    check(lay20 && lay21, "布局表含 2.0 与 2.1");

    printf("=== 1. 2.0 全流程：两声道不同延迟 → valid/peakMs/SNR/无警告/进度回调 ===\n");
    MeasurementSessionResult res1;
    {
        VirtualRoomTransport tx;
        tx.roomIrs = { make_room_ir(fs, 480), make_room_ir(fs, 744) };   // L 10 ms / R 15.5 ms
        MeasurementSession sess(spec, *lay20);                           // 默认 −20 dBFS
        std::vector<std::pair<int, int>> steps;
        std::vector<std::string> names;
        tx.onProgress = [&](int step, int total, const char* name) {
            steps.push_back({ step, total });
            names.push_back(name ? name : "");
        };
        res1 = sess.run(tx);
        char b[256];
        std::snprintf(b, sizeof(b), "(L peak %.3f ms / SNR %.1f dB，R peak %.3f ms / SNR %.1f dB)",
                      res1.channels[0].peakMs, res1.channels[0].snrDb,
                      res1.channels[1].peakMs, res1.channels[1].snrDb);
        printf("  %s\n", b);
        check(res1.channels.size() == 2, "两声道都测到", b);
        check(res1.channels[0].valid && res1.channels[1].valid, "两声道 valid", b);
        check(std::fabs(res1.channels[0].peakMs - 10.0) <= 0.5, "L peakMs = 10 ms ±0.5", b);
        check(std::fabs(res1.channels[1].peakMs - 15.5) <= 0.5, "R peakMs = 15.5 ms ±0.5", b);
        check(res1.channels[0].snrDb > 30.0 && res1.channels[1].snrDb > 30.0, "两声道 SNR > 30 dB", b);
        check(res1.warnings.empty(), "无警告", join(res1.warnings));
        check(!res1.aborted, "未中止", b);
        check(steps.size() == 2, "进度回调次数 = 声道数", b);
        check(steps.size() == 2 && steps[0].first == 1 && steps[0].second == 2 &&
              steps[1].first == 2 && steps[1].second == 2, "stepIndex/totalSteps 递增正确", b);
        check(names.size() == 2 && names[0] == "L" && names[1] == "R", "声道名进进度回调", b);
        check(tx.calls == 2, "每声道恰好一次播放采集", b);
    }

    printf("=== 2. 低信噪比：R 声道噪声 −10 dBFS(RMS) → SNR 告警，L 不误报 ===\n");
    {
        VirtualRoomTransport tx;
        tx.roomIrs = { make_room_ir(fs, 480), make_room_ir(fs, 744) };
        tx.channelNoiseDbfs = { -80.0, -10.0 };
        MeasurementSession sess(spec, *lay20);
        const auto res = sess.run(tx);
        char b[160];
        std::snprintf(b, sizeof(b), "(L SNR %.1f dB，R SNR %.1f dB)",
                      res.channels[0].snrDb, res.channels[1].snrDb);
        printf("  %s\n", b);
        printf("  warnings: %s\n", join(res.warnings).c_str());
        check(res.channels.size() == 2 && res.channels[1].valid, "R 声道完成测量（告警≠无效）", b);
        check(res.channels[1].snrDb < 20.0, "R snrDb < 20", b);
        check(contains(res.warnings, "R 环境噪声偏大"), "warnings 含 R『环境噪声偏大』",
              join(res.warnings));
        check(!contains(res.warnings, "L 环境噪声偏大"), "L 声道无 SNR 误报", join(res.warnings));
        check(res.channels[0].snrDb > 30.0, "L 声道 SNR 不受影响", b);
    }

    printf("=== 3. 削波：R 声道输出 ×1.5 → 削波告警，L 不误报 ===\n");
    {
        // 扫频电平 −9 dBFS：本房间低频 |H|≈+7.4 dB（+4 dB 架 × 梳状同相 +3.5 dB 功率），
        // L 峰值 ≈ 0.355×2.36 ≈ 0.84（预警线下），R ×1.5 ≈ 1.26（≥0.999 触发）
        VirtualRoomTransport tx;
        tx.roomIrs = { make_room_ir(fs, 480), make_room_ir(fs, 744) };
        tx.channelExtraGain = { 1.0, 1.5 };
        MeasurementSession sess(spec, *lay20, -9.0);
        const auto res = sess.run(tx);
        printf("  warnings: %s\n", join(res.warnings).c_str());
        check(contains(res.warnings, "R 录音接近削波"), "warnings 含 R『削波』",
              join(res.warnings));
        check(!contains(res.warnings, "L 录音接近削波"), "L 声道无削波误报", join(res.warnings));
        check(res.warnings.size() == 1, "恰好一条警告（无 SNR/极性误报）", join(res.warnings));
        check(res.channels[1].valid, "削波声道仍 valid（告警是重测建议，不是结构失败）");
    }

    printf("=== 4. 2.1 布局 + 超低取反：延迟 12 ms 恢复 + 极性告警 ===\n");
    {
        VirtualRoomTransport tx;
        tx.roomIrs = { delta_ir(fs, 480), delta_ir(fs, 480),
                       delta_ir(fs, 1056, 0.5, /*invert=*/true) };   // 主 10 ms / 超低 22 ms 反接 −6 dB
        MeasurementSession sess(spec, *lay21);
        const auto res = sess.run(tx);
        char b[160];
        std::snprintf(b, sizeof(b), "(delay %.1f 样本 = %.3f ms，gain %+.3f dB，警告 %zu 条)",
                      res.subAlign.delaySamples, res.subAlign.delaySamples / fs * 1000.0,
                      res.subAlign.gainDb, res.warnings.size());
        printf("  %s\n", b);
        printf("  warnings: %s\n", join(res.warnings).c_str());
        check(res.channels.size() == 3 && res.channels[2].isSub, "三声道，LFE 识别为超低", b);
        check(res.channels[0].valid && res.channels[1].valid && res.channels[2].valid,
              "三声道全部 valid", b);
        check(std::fabs(res.subAlign.delaySamples - 576.0) <= 24.0, "subAlign 延迟 = 12 ms ±0.5 ms", b);
        check(contains(res.warnings, "相位疑似反接"), "warnings 含『相位疑似反接』",
              join(res.warnings));
        // 带限增益真值：主 δ=1.0、超低 δ=0.5 → 带内能量比 4 → +6.02 dB（C1 §4 同语义）
        check(std::fabs(res.subAlign.gainDb - 6.02) <= 0.5, "subAlign 增益 = +6 dB ±0.5", b);
    }

    printf("=== 5. 取消：第 1 声道测完后 cancelled → aborted、保留已完成声道 ===\n");
    {
        VirtualRoomTransport tx;
        tx.roomIrs = { make_room_ir(fs, 480), make_room_ir(fs, 744) };
        tx.cancelled = [&tx]() { return tx.calls >= 1; };
        MeasurementSession sess(spec, *lay20);
        const auto res = sess.run(tx);
        char b[96];
        std::snprintf(b, sizeof(b), "(aborted=%d，channels=%zu，播放采集 %d 次)",
                      (int)res.aborted, res.channels.size(), tx.calls);
        printf("  %s\n", b);
        check(res.aborted, "aborted = true", b);
        check(res.channels.size() == 1, "channels.size() == 1", b);
        check(!res.channels.empty() && res.channels[0].valid, "已完成声道仍有效", b);
    }

    printf("=== 6. 校正闭环：build_corrections(tilt −1 dB/oct) → conv 闭环 vs target ≤2 dB ===\n");
    CorrectionSet cs1;
    {
        const TargetCurve tgt = TargetCurve::tilt(-1.0);
        cs1 = build_corrections(res1, tgt, CorrectionOpts{}, fs);
        check(cs1.channelNames.size() == 2 && cs1.correctionIrs.size() == 2,
              "两声道校正 IR 齐全", b2s(cs1.channelNames));
        check(cs1.fs == 48000 && cs1.taps == 8192, "fs/taps 如实记录", "");
        check(cs1.channelNames.size() == 2 && cs1.channelNames[0] == "L" &&
              cs1.channelNames[1] == "R", "声道名与测量结果一一对应", b2s(cs1.channelNames));
        check(cs1.subDelaySamples == 0.0 && cs1.subGainDb == 0.0, "无超低布局对齐参数为 0", "");
        for (size_t c = 0; c < cs1.correctionIrs.size(); ++c) {
            const auto closed = fft_convolve(cs1.correctionIrs[c], res1.channels[c].roomIr);
            auto magC = ir_magnitude_db(closed, fs);
            psychoacoustic_smooth(magC, fs);                    // 与校正器设计同宽（C1 坑 4）
            const auto tt = target_curve_db(tgt, fs, magC.size());
            double atHz = 0.0;
            const double d = max_diff_db(magC, tt, fs, 100.0, 10000.0, &atHz);
            char b[160];
            std::snprintf(b, sizeof(b), "(%s：max|Δ| = %.4f dB @ %.0f Hz)",
                          cs1.channelNames[c].c_str(), d, atHz);
            printf("  %s\n", b);
            check(d <= 2.0, cs1.channelNames[c] + " 闭环平滑幅度与 target 差 ≤2 dB", b);
        }
    }

    printf("=== 7. 导出：每声道 32-bit float WAV，RIFF 头正确 ===\n");
    {
        const fs::path dir = fs::temp_directory_path() / "rta_c2_export_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        check(fs::create_directories(dir, ec), "临时目录创建", dir.string());
        const std::string prefix = dir.string() + "/";
        const std::string err = export_corrections_wav(cs1, prefix);
        check(err.empty(), "导出无错误", err);
        for (const auto& name : cs1.channelNames) {
            const fs::path p = dir / (name + ".wav");
            char head[44] = {};
            size_t got = 0;
            long fsz = 0;
            if (std::FILE* f = std::fopen(p.string().c_str(), "rb")) {
                got = std::fread(head, 1, sizeof(head), f);
                std::fseek(f, 0, SEEK_END);
                fsz = std::ftell(f);
                std::fclose(f);
            }
            const uint32_t dataLen = (uint32_t)(uint8_t)head[40] |
                                     ((uint32_t)(uint8_t)head[41] << 8) |
                                     ((uint32_t)(uint8_t)head[42] << 16) |
                                     ((uint32_t)(uint8_t)head[43] << 24);
            char b[192];
            std::snprintf(b, sizeof(b), "(%s：%ld 字节，RIFF=%.4s WAVE=%.4s dataLen=%u)",
                          name.c_str(), fsz, head, head + 8, dataLen);
            printf("  %s\n", b);
            check(got == 44 && fsz >= 44, name + " WAV ≥44 字节", b);
            check(std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "WAVE", 4) == 0,
                  name + " RIFF/WAVE 头正确", b);
            check(dataLen == (uint32_t)cs1.taps * 4, name + " data 块长 = taps×4 字节", b);
        }
        fs::remove_all(dir, ec);
        check(!fs::exists(dir), "临时目录已删除", dir.string());
    }

    printf("=== 8. 半套拒绝：任一声道无效 → build_corrections 返回空集 ===\n");
    {
        auto res8 = res1;
        res8.channels[0].valid = false;
        const auto cs8 = build_corrections(res8, TargetCurve::tilt(-1.0), CorrectionOpts{}, fs);
        check(cs8.channelNames.empty() && cs8.correctionIrs.empty(),
              "无效声道使整批拒绝（channelNames/correctionIrs 均空）", "");
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
