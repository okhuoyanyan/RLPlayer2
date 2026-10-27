// test_fake_lossless.cpp — 音频体检 B1a：假无损检测合成信号验证
// 全部信号现场合成、完全离线：砖墙低通用项目现成的 Kaiser 窗函数法 FIR（src/dsp/filter_design.h）
// 造出来，缓坡滚降用 Butterworth 双二阶级联 —— 判据确定性可复现。
// 关键反误报用例（第 4 节）必须证明「陡沿门」确实把温和滚降拦在判假之外。
#include "analysis/fake_lossless.h"
#include "dsp/filter_design.h"
#include "dsp/matrix.h"
#include "dsp/dither.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace rta::analysis;
using namespace rta::dsp;
using rta::dsp::kPi;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

static const double kFs = 44100.0;
static const size_t kN = 882000;          // 20 s：够均匀铺满 12 个 65536 样窗
static const int kTaps = 511;             // 奇数抽头 Kaiser FIR（44.1k 下过渡带 ≈550 Hz）

// 确定性高斯白噪，峰值归一到 target
static std::vector<std::vector<double>> make_noise(int channels, uint64_t seed, double target) {
    std::vector<std::vector<double>> ch((size_t)channels, std::vector<double>(kN));
    for (int c = 0; c < channels; ++c) {
        Rng rng(seed + (uint64_t)c * 7919ull);
        double pk = 0.0;
        for (size_t i = 0; i < kN; ++i) {
            ch[(size_t)c][i] = rng.gauss();
            pk = std::max(pk, std::fabs(ch[(size_t)c][i]));
        }
        const double g = target / pk;
        for (size_t i = 0; i < kN; ++i) ch[(size_t)c][i] *= g;
    }
    return ch;
}

static std::vector<const double*> ptrs(const std::vector<std::vector<double>>& ch) {
    std::vector<const double*> p;
    for (const auto& c : ch) p.push_back(c.data());
    return p;
}

// 直接卷积 FIR（h 已归一直流增益）；只做因果部分，头部瞬态远小于分析窗
static void fir_apply(std::vector<double>& x, const std::vector<double>& h) {
    std::vector<double> y(x.size(), 0.0);
    for (size_t i = 0; i < x.size(); ++i) {
        double s = 0.0;
        const size_t kmax = std::min(h.size(), i + 1);
        for (size_t k = 0; k < kmax; ++k) s += h[k] * x[i - k];
        y[i] = s;
    }
    x.swap(y);
}

// Kaiser 砖墙低通（fc 以 Nyquist 归一）
static std::vector<double> brickwall(double fcHz) {
    LowpassSpec spec;
    spec.taps = kTaps;
    spec.cutoff = fcHz / (kFs * 0.5);
    spec.attenDb = 100.0;
    return design_lowpass(spec);
}


// ---- 增量接口的比对辅助 ----
static bool same_bits(double a, double b) {
    if (a == b) return true;                              // 含 +0/-0
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}
// 逐位相同 = 五个输出字段全等（reason 按字符串逐字比较）
static bool same_check(const LosslessCheck& a, const LosslessCheck& b) {
    return a.suspected == b.suspected && a.fake24bit == b.fake24bit
        && same_bits(a.cutoffHz, b.cutoffHz) && same_bits(a.confidence, b.confidence)
        && a.reason == b.reason
        // 2026-10-27：判定档与判据量也纳入"逐位一致"契约（UI 直接显示它们）
        && a.fakeVerdict == b.fakeVerdict && same_bits(a.edgeDb, b.edgeDb)
        && a.windows == b.windows && same_bits(a.windowFill, b.windowFill)
        && same_bits(a.cutoffRatio, b.cutoffRatio) && a.guardBins == b.guardBins;
}
// 按 B 帧一块喂满整段；planOk 返回 planMatched()
static LosslessCheck fed_chunked(const std::vector<std::vector<double>>& ch, size_t n, double fs,
                                 int srcBits, size_t B, bool* planOk = nullptr) {
    LosslessAccumulator acc;
    acc.begin(n, (int)ch.size(), fs, srcBits);
    for (size_t pos = 0; pos < n; pos += B) {
        const size_t m = std::min(B, n - pos);
        std::vector<const double*> sub(ch.size());
        for (size_t c = 0; c < ch.size(); ++c) sub[c] = ch[c].data() + pos;
        acc.add_planar(sub.data(), m);
    }
    const LosslessCheck c = acc.finalize();
    if (planOk) *planOk = acc.planMatched();
    return c;
}

static std::string brief(const LosslessCheck& c) {
    char b[256];
    std::snprintf(b, sizeof(b), "(cutoff %.1f Hz, conf %.3f, suspected %d, fake24 %d) %s",
                  c.cutoffHz, c.confidence, c.suspected ? 1 : 0, c.fake24bit ? 1 : 0,
                  c.reason.c_str());
    return std::string(b);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== 1. 全带白噪 @44.1k → 不判假、无截止 ===\n");
    {
        auto ch = make_noise(2, 0xDEADBEEFull, 0.5);
        auto p = ptrs(ch);
        const LosslessCheck c = check_lossless(p.data(), 2, kN, kFs, 16);
        printf("  %s\n", brief(c).c_str());
        check(!c.suspected, "suspected = false");
        check(c.cutoffHz == 0.0, "cutoffHz = 0（全带无截止）");
    }

    printf("\n=== 2. 16 kHz 砖墙低通白噪 → 判假、截止 16000±800 ===\n");
    {
        auto ch = make_noise(2, 0x12345678ull, 0.5);
        fir_apply(ch[0], brickwall(16000.0));
        fir_apply(ch[1], brickwall(16000.0));
        auto p = ptrs(ch);
        const LosslessCheck c = check_lossless(p.data(), 2, kN, kFs, 16);
        printf("  %s\n", brief(c).c_str());
        check(c.suspected, "suspected = true");
        check(std::fabs(c.cutoffHz - 16000.0) <= 800.0, "cutoffHz = 16000 ±800", brief(c));
        check(c.confidence >= 0.5, "confidence ≥ 0.5");
    }

    printf("\n=== 3. 19 kHz 砖墙低通白噪 → 判假、截止 19000±800 ===\n");
    {
        auto ch = make_noise(2, 0x9ABCDEF0ull, 0.5);
        fir_apply(ch[0], brickwall(19000.0));
        fir_apply(ch[1], brickwall(19000.0));
        auto p = ptrs(ch);
        const LosslessCheck c = check_lossless(p.data(), 2, kN, kFs, 16);
        printf("  %s\n", brief(c).c_str());
        check(c.suspected, "suspected = true");
        check(std::fabs(c.cutoffHz - 19000.0) <= 800.0, "cutoffHz = 19000 ±800", brief(c));
    }

    printf("\n=== 4. 缓和滚降（−3 dB @18k、2 阶 Butterworth 缓坡）→ 不判假 ===\n");
    {
        auto ch = make_noise(2, 0x55AA55AAull, 0.5);
        // 每声道独立的滤波器实例 —— 共享实例会让状态机按 2 倍速率步进，
        // 输出变成抽取后的多相混合（高频镜像只降 10 dB，完全污染频谱）
        Biquad lp0, lp1;
        biquad_design::lowpass(lp0, kFs, 18000.0, 0.70710678);
        biquad_design::lowpass(lp1, kFs, 18000.0, 0.70710678);
        for (size_t i = 0; i < kN; ++i) { ch[0][i] = lp0.process(ch[0][i]); ch[1][i] = lp1.process(ch[1][i]); }
        auto p = ptrs(ch);
        const LosslessCheck c = check_lossless(p.data(), 2, kN, kFs, 16);
        printf("  %s\n", brief(c).c_str());
        check(!c.suspected, "suspected = false（缓坡必须放过）");
    }

    printf("\n=== 4b. 截止可见但下降沿缓和（4 阶 Butterworth ≈30 dB/oct，−40 dB 点 ≈12k）→ 陡沿门拦下 ===\n");
    {
        // IIR 在接近奈奎斯特处因双线性零点而急剧变陡，「有截止但边沿缓和」的场景必须把
        // 转折频率放低才成立。4 阶 Butterworth 在 −40 dB 点附近局部斜率只有 ~30 dB/oct，
        // 换算 1/6 倍频程仅 ~5 dB，过不了 >12 dB 陡沿门 —— 专门证明「有截止≠判假」。
        auto ch = make_noise(2, 0x77777777ull, 0.5);
        const double f0 = 3400.0;
        const double Q[2] = { 0.541196, 1.306563 };
        Biquad bq[2][2];
        for (int k = 0; k < 2; ++k) {
            biquad_design::lowpass(bq[k][0], kFs, f0, Q[k]);
            biquad_design::lowpass(bq[k][1], kFs, f0, Q[k]);
        }
        for (size_t i = 0; i < kN; ++i) {
            ch[0][i] = bq[0][1].process(bq[0][0].process(ch[0][i]));
            ch[1][i] = bq[1][1].process(bq[1][0].process(ch[1][i]));
        }
        auto p = ptrs(ch);
        const LosslessCheck c = check_lossless(p.data(), 2, kN, kFs, 16);
        printf("  %s\n", brief(c).c_str());
        check(!c.suspected, "suspected = false（陡沿门生效）");
        check(c.cutoffHz > 9000.0 && c.cutoffHz < 16000.0, "截止确实被检出（9000 < cutoff < 16000）", brief(c));
        check(c.reason.find("缓和") != std::string::npos, "拒绝原因 = 下降沿缓和（不是没检出）");
    }

    printf("\n=== 5. fake24bit：16-bit 内容 <<8 装入 24-bit → true；加 1 LSB 抖动 → false ===\n");
    {
        // 5a：确定性 16-bit 量化，再左移 8 位装 24-bit 容器
        auto ch = make_noise(1, 0x0F0F0F0Full, 0.5);
        Rng rng(42);
        for (size_t i = 0; i < kN; ++i) {
            long long v16 = (long long)std::llround(ch[0][i] * 32767.0);
            v16 = std::max(-32768ll, std::min(32767ll, v16));
            ch[0][i] = (double)(v16 << 8) / 8388608.0;
        }
        auto p = ptrs(ch);
        const LosslessCheck a = check_lossless(p.data(), 1, kN, kFs, 24);
        printf("  5a %s\n", brief(a).c_str());
        check(a.fake24bit, "fake24bit = true（低 8 位恒零）");

        // 5b：同信号叠加 ±1 LSB(24bit) 抖动 —— 低 8 位被激活，占比跌到 ~1/3
        for (size_t i = 0; i < kN; ++i)
            ch[0][i] += (double)((long long)(rng.next() % 3) - 1) / 8388608.0;
        const LosslessCheck b2 = check_lossless(p.data(), 1, kN, kFs, 24);
        printf("  5b %s\n", brief(b2).c_str());
        check(!b2.fake24bit, "加 1 LSB 抖动后 fake24bit = false");
    }


    printf("\n=== 6. 增量接口 vs 整段接口：逐位一致（3 种素材 × 块大小 1/1000/4096/65536/262144）===\n");
    {
        const std::vector<size_t> blocks = {1, 1000, 4096, 65536, 262144};
        // 三种素材分别压不同的判据通路：判假（16k 砖墙）/ 不判假（全带）/ fake24（低 8 位恒零）
        auto noise16k = make_noise(2, 0x12345678ull, 0.5);
        fir_apply(noise16k[0], brickwall(16000.0));
        fir_apply(noise16k[1], brickwall(16000.0));
        auto flat = make_noise(2, 0xDEADBEEFull, 0.5);
        auto q16 = make_noise(1, 0x0F0F0F0Full, 0.5);
        for (size_t i = 0; i < kN; ++i) {
            long long v16 = (long long)std::llround(q16[0][i] * 32767.0);
            v16 = std::max(-32768ll, std::min(32767ll, v16));
            q16[0][i] = (double)(v16 << 8) / 8388608.0;
        }
        struct Case { const char* name; const std::vector<std::vector<double>>* sig; int bits; };
        const Case cases[] = {
            { "16 kHz 砖墙（应判假）", &noise16k, 16 },
            { "全带白噪（不应判假）", &flat, 16 },
            { "16-bit<<8 装 24-bit（fake24）", &q16, 24 },
        };
        for (const Case& cs : cases) {
            auto p = ptrs(*cs.sig);
            const LosslessCheck ref = check_lossless(p.data(), (int)cs.sig->size(), kN, kFs, cs.bits);
            bool allSame = true, allPlanned = true;
            double maxCutDiff = 0.0, maxConfDiff = 0.0;
            for (size_t B : blocks) {
                bool ok = false;
                const LosslessCheck c = fed_chunked(*cs.sig, kN, kFs, cs.bits, B, &ok);
                allSame = allSame && same_check(ref, c);
                allPlanned = allPlanned && ok;
                maxCutDiff = std::max(maxCutDiff, std::fabs(c.cutoffHz - ref.cutoffHz));
                maxConfDiff = std::max(maxConfDiff, std::fabs(c.confidence - ref.confidence));
            }
            char b[320];
            std::snprintf(b, sizeof(b),
                          "(%s: cutoff %.17g / conf %.17g / sus %d / fake24 %d, 块大小 %zu 种, max|Δcutoff| = %g, max|Δconf| = %g)",
                          cs.name, ref.cutoffHz, ref.confidence, (int)ref.suspected, (int)ref.fake24bit,
                          blocks.size(), maxCutDiff, maxConfDiff);
            check(allSame && allPlanned, std::string("逐位一致: ") + cs.name, b);
        }
    }

    printf("\n=== 7. 计划（总帧数）与实喂帧数不符 → planMatched() = false（调用方必须回退整段）===\n");
    {
        auto sig = make_noise(2, 0x5A5A5A5Aull, 0.5);
        {
            LosslessAccumulator acc;
            acc.begin(kN, 2, kFs, 16);
            bool ok = false;
            const LosslessCheck c = fed_chunked(sig, kN, kFs, 16, 65536, &ok);
            check(ok, "声明总帧数 == 实喂帧数 → planMatched() = true");
            (void)c;
        }
        {
            LosslessAccumulator acc;
            acc.begin(kN * 2, 2, kFs, 16);      // 声明两倍（模拟"用错总长"）
            std::vector<const double*> sub{ sig[0].data(), sig[1].data() };
            acc.add_planar(sub.data(), kN);
            check(!acc.planMatched(), "声明总帧数偏大 → planMatched() = false");
            check(acc.framesSeen() == kN, "framesSeen() 如实报实喂帧数", std::to_string(acc.framesSeen()));
        }
        {
            LosslessAccumulator acc;
            acc.begin(kN, 2, kFs, 16);
            std::vector<const double*> sub{ sig[0].data(), sig[1].data() };
            acc.add_planar(sub.data(), kN / 2);   // 短读（截断/IO 失败）
            check(!acc.planMatched(), "短读（帧数不足）→ planMatched() = false");
        }
        {
            LosslessAccumulator acc;
            acc.begin(0, 2, kFs, 16);             // 空素材
            const LosslessCheck c = acc.finalize();
            check(c.reason == std::string("样本不足，无法分析") && !acc.planMatched(),
                  "总帧数 0 → 与整段接口同一句人话原因", c.reason);
        }
    }

    printf("\n=== 8. 不足一个谱窗（n < 65536）× 分块 → 与整段一致 ===\n");
    {
        const size_t nSmall = 30011;              // 故意不是 2 的幂、也不是块大小的倍数
        std::vector<std::vector<double>> sig(2, std::vector<double>(nSmall));
        Rng rng(777);
        for (int c = 0; c < 2; ++c)
            for (size_t i = 0; i < nSmall; ++i) sig[(size_t)c][i] = 0.4 * rng.gauss();
        auto p = ptrs(sig);
        const LosslessCheck ref = check_lossless(p.data(), 2, nSmall, kFs, 16);
        bool allSame = true;
        for (size_t B : {size_t(1), size_t(997), size_t(65536)}) {
            bool ok = false;
            const LosslessCheck c = fed_chunked(sig, nSmall, kFs, 16, B, &ok);
            allSame = allSame && same_check(ref, c) && ok;
        }
        char b[192];
        std::snprintf(b, sizeof(b), "(n=%zu: cutoff %.17g, conf %.17g, %s)", nSmall, ref.cutoffHz,
                      ref.confidence, ref.reason.c_str());
        check(allSame, "单窗（n<65536）分块与整段逐位一致", b);
    }

    printf("\n=== 9. 同一素材 4 种长度 → 同一结论（P1：陡沿逐窗各算一次、取中位数）===\n");
    {
        // 8 阶（4 个双二阶级联）16 kHz 低通白噪 —— 与标定素材 lpb_16000 同一条通路。
        // 这类"截止不算太低、陡沿也不算太陡"的素材正是旧实现翻车的场景：
        // 先把 12 个窗的幅度谱平均、再量一次陡沿 ⟹ 窗越多陡沿越低，30 s 反而漏报。
        const size_t n30 = (size_t)(30.0 * kFs);
        std::vector<std::vector<double>> sig(2, std::vector<double>(n30));
        for (int c = 0; c < 2; ++c) {
            Rng rng(0xC0FFEEull + (uint64_t)c * 7919ull);
            for (size_t i = 0; i < n30; ++i) sig[(size_t)c][i] = 0.4 * rng.gauss();
        }
        for (int c = 0; c < 2; ++c) {
            const double Q[2] = { 0.541196, 1.306563 };
            Biquad bq[4];
            biquad_design::lowpass(bq[0], kFs, 16000.0, Q[0]);
            biquad_design::lowpass(bq[1], kFs, 16000.0, Q[1]);
            biquad_design::lowpass(bq[2], kFs, 16000.0, Q[0]);
            biquad_design::lowpass(bq[3], kFs, 16000.0, Q[1]);
            for (size_t i = 0; i < n30; ++i) {
                double v = sig[(size_t)c][i];
                for (int k = 0; k < 4; ++k) v = bq[k].process(v);
                sig[(size_t)c][i] = v;
            }
        }
        std::vector<std::pair<double, LosslessCheck>> got;
        for (double L : {30.0, 10.0, 3.0}) {
            const size_t n = (size_t)(L * kFs);
            std::vector<const double*> p{ sig[0].data(), sig[1].data() };
            got.push_back({ L, check_lossless(p.data(), 2, n, kFs, 16) });
        }
        bool sameVerdict = true, detected = true;
        std::string detail;
        for (const auto& g : got) {
            const LosslessCheck& c = g.second;
            sameVerdict = sameVerdict && (c.fakeVerdict == got[0].second.fakeVerdict);
            detected = detected && c.fakeVerdict != kFakeVerdictNone && c.fakeVerdict != kFakeVerdictInconclusive;
            char b[192];
            std::snprintf(b, sizeof(b), "%.0fs: cut %.0f conf %.3f edge %.1f win %d v %d | ", g.first,
                          c.cutoffHz, c.confidence, c.edgeDb, c.windows, c.fakeVerdict);
            detail += b;
        }
        check(sameVerdict && detected, "30/10/3 s 三个长度给出同一结论（且都算「检出」）", detail);
        // 陡沿必须与窗数基本无关（旧实现 2 窗 21.9 → 12 窗 18.1）
        {
            const double eShort = got[2].second.edgeDb, eLong = got[0].second.edgeDb;
            char b[160];
            std::snprintf(b, sizeof(b), "(3 s %.1f dB vs 30 s %.1f dB, 差 %+.1f dB)", eShort, eLong, eLong - eShort);
            check(std::fabs(eLong - eShort) <= 3.0, "陡沿与片段长度无关（|Δ| ≤ 3 dB）", b);
        }
        // 0.6 s：谱窗填充 <50% ⟹ 必须给「证据不足」，既不许判假、也不许说"未发现"
        {
            const size_t n = (size_t)(0.6 * kFs);
            std::vector<const double*> p{ sig[0].data(), sig[1].data() };
            const LosslessCheck c = check_lossless(p.data(), 2, n, kFs, 16);
            char b[256];
            std::snprintf(b, sizeof(b), "(fill %.2f, cut %.0f, edge %.1f, conf %.3f, v %d) %s",
                          c.windowFill, c.cutoffHz, c.edgeDb, c.confidence, c.fakeVerdict, c.reason.c_str());
            check(!c.suspected && c.fakeVerdict == kFakeVerdictInconclusive,
                  "0.6 s（fill<0.5）→ 证据不足，不判假", b);
            check(c.reason.find("不构成判据") != std::string::npos, "0.6 s 的 reason 写明「不构成判据」", c.reason);
        }
        // 1.0 s：fill 在 0.5~0.9 之间 ⟹ 置信度打七折，reason 必须写明（P4 第二档）
        {
            const size_t n = (size_t)(1.0 * kFs);
            std::vector<const double*> p{ sig[0].data(), sig[1].data() };
            const LosslessCheck c = check_lossless(p.data(), 2, n, kFs, 16);
            char b[256];
            std::snprintf(b, sizeof(b), "(fill %.2f, conf %.3f) %s", c.windowFill, c.confidence, c.reason.c_str());
            check(c.windowFill >= 0.5 && c.windowFill < 0.9 && c.reason.find("折算") != std::string::npos,
                  "1.0 s（fill 0.5~0.9）→ reason 写明置信度已折算", b);
        }
    }

    printf("\n=== 10. 两档相对奈奎斯特（P2）：20 kHz 8 阶 ⟹ 带宽受限；12 kHz 8 阶 ⟹ 判有损 ===\n");
    {
        // 用 8 阶（4 个双二阶级联）而不是 511 抽头 Kaiser 砖墙：真实的母带低通/老录音
        // 就是这个陡度（20 kHz 处实测陡沿 20~25 dB），而理想砖墙（60~70 dB）在现实里不存在。
        auto cascade8 = [&](std::vector<std::vector<double>>& v, double fc) {
            for (size_t c = 0; c < v.size(); ++c) {
                const double Q[2] = { 0.541196, 1.306563 };
                Biquad bq[4];
                biquad_design::lowpass(bq[0], kFs, fc, Q[0]);
                biquad_design::lowpass(bq[1], kFs, fc, Q[1]);
                biquad_design::lowpass(bq[2], kFs, fc, Q[0]);
                biquad_design::lowpass(bq[3], kFs, fc, Q[1]);
                for (size_t i = 0; i < v[c].size(); ++i) {
                    double x = v[c][i];
                    for (int k = 0; k < 4; ++k) x = bq[k].process(x);
                    v[c][i] = x;
                }
            }
        };
        // 8 阶 Butterworth 的 −40 dB 点在 fc 的 1.19 倍处（白噪实测），所以 fc=15.6k 的
        // 截止落在 ≈18.6 kHz = 0.84×Nyq —— 正是"母带低通/升频"那一档，也是旧实现被
        // 20500 绝对门一刀切成"未发现"的区间。
        auto v20 = make_noise(2, 0xBEEF0001ull, 0.5);
        cascade8(v20, 15600.0);
        auto p20 = ptrs(v20);
        const LosslessCheck c20 = check_lossless(p20.data(), 2, kN, kFs, 16);
        printf("  band-limited %s\n", brief(c20).c_str());
        char b[224];
        std::snprintf(b, sizeof(b), "(cutoff %.0f Hz = %.3f×Nyq, edge %.1f dB, conf %.3f)",
                      c20.cutoffHz, c20.cutoffRatio, c20.edgeDb, c20.confidence);
        check(!c20.suspected && c20.fakeVerdict == kFakeVerdictBandLimited,
              "0.84×Nyq 的陡沿 → 带宽受限，不判假（旧实现被 20500 绝对门漏成「未发现」）", b);
        check(c20.reason.find("带宽受限") != std::string::npos, "reason 写明「有效带宽受限」", c20.reason);

        auto v12 = make_noise(2, 0xBEEF0002ull, 0.5);
        cascade8(v12, 12000.0);
        auto p12 = ptrs(v12);
        const LosslessCheck c12 = check_lossless(p12.data(), 2, kN, kFs, 16);
        printf("  lossy-band %s\n", brief(c12).c_str());
        std::snprintf(b, sizeof(b), "(cutoff %.0f Hz = %.3f×Nyq, edge %.1f dB, conf %.3f)",
                      c12.cutoffHz, c12.cutoffRatio, c12.edgeDb, c12.confidence);
        check(c12.suspected && c12.fakeVerdict == kFakeVerdictLossy,
              "12 kHz 8 阶（<0.78×Nyq）→ 判有损（旧实现漏报）", b);
    }

    printf("\n=== 11. 纯音/窄带护栏（P3）：997 Hz 主导的窄带素材 → 不判假 ===\n");
    {
        // 997 Hz −6 dBFS + −70 dBFS 底噪，再 1.15 kHz 砖墙低通：
        // 这就是标定报告里"检出 1172 Hz、置信 1.00 ⟹ 有损"那类误报的成因（截止远低于 6 kHz）。
        const size_t n = (size_t)(5.0 * kFs);
        std::vector<std::vector<double>> ch(2, std::vector<double>(n));
        Rng rng(0x997997ull);
        for (size_t i = 0; i < n; ++i) {
            const double tone = 0.5011872336 * std::sin(2.0 * kPi * 997.0 * (double)i / kFs);
            ch[0][i] = tone + 3.16e-4 * rng.gauss();
            ch[1][i] = 0.0;
        }
        fir_apply(ch[0], brickwall(1150.0));
        std::vector<const double*> p{ ch[0].data(), nullptr };
        const LosslessCheck c = check_lossless(p.data(), 2, n, kFs, 16);
        printf("  %s\n", brief(c).c_str());
        char b[224];
        std::snprintf(b, sizeof(b), "(cutoff %.0f Hz, edge %.1f dB, guard %d bin, conf %.3f)",
                      c.cutoffHz, c.edgeDb, c.guardBins, c.confidence);
        check(!c.suspected && c.fakeVerdict == kFakeVerdictNone,
              "997 Hz 主导 + 1.15 kHz 陡沿 → 未发现（不判假）", b);
        check(c.reason.find("6 kHz") != std::string::npos, "reason 说明「低于 6 kHz 不构成判据」", c.reason);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
