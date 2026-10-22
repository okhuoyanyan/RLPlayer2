// test_conv_matrix.cpp — 卷积引擎 / 双二阶 EQ / 延迟 / Matrix 路由验证
// 判据设计（遵守"测试通过 != 测对了东西"）：
//   卷积：与直接线性卷积逐样本比对（同一输入、同一 IR），必须几乎完全一致
//   双二阶：解析幅频响应 与 脉冲响应实测幅频 必须一致；peak 滤波器在 f0 处必须等于设定增益
//   延迟：脉冲必须出现在恰好 n 个样本之后
//   Matrix：纯算术，逐样本精确
#include "dsp/convolution.h"
#include "dsp/matrix.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <string>
#include <random>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // 崩溃时不丢输出，便于定位
    printf("=== 1. 分块 FFT 卷积 vs 直接卷积 ===\n");
    {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<double> u(-1.0, 1.0);

        // 室内校正风格的 IR：长度 32768（远超块大小）
        const size_t irLen = 32768;
        std::vector<double> ir(irLen);
        for (size_t i = 0; i < irLen; ++i) {
            const double t = (double)i / (double)irLen;
            ir[i] = u(rng) * std::exp(-6.0 * t);            // 指数衰减噪声 IR
        }

        const size_t N = 40000;
        std::vector<double> x(N);
        for (size_t i = 0; i < N; ++i) x[i] = u(rng);

        const auto ref = direct_convolve(x, ir);   // 只算一次，三种块大小共用

        for (size_t B : {size_t(256), size_t(1024), size_t(4096)}) {
            PartitionedConvolver pc(ir, B);
            std::vector<double> y(N, 0.0);
            std::vector<double> blk(B), oblk(B);
            for (size_t off = 0; off + B <= N; off += B) {
                for (size_t i = 0; i < B; ++i) blk[i] = x[off + i];
                pc.process_block(blk.data(), oblk.data());
                for (size_t i = 0; i < B; ++i) y[off + i] = oblk[i];
            }
            // 只比对"被整块处理过"的区间：N 不是块大小的整数倍时，尾部不足一块的样本
            // 根本没有进入卷积器，拿它们去比对必然产生假失败（曾因此报出 72% 相对误差）
            const size_t processed = (N / B) * B;
            double maxDiff = 0.0, maxRef = 0.0;
            for (size_t i = 0; i < processed; ++i) {
                maxDiff = std::max(maxDiff, std::fabs(y[i] - ref[i]));
                maxRef  = std::max(maxRef,  std::fabs(ref[i]));
            }
            char buf[200];
            std::snprintf(buf, sizeof(buf), "(块 %zu, %zu 段, 群延迟 %zu, 相对误差 %.3e)",
                          B, pc.partitions(), pc.latency(), maxDiff / (maxRef + 1e-300));
            check(maxDiff / (maxRef + 1e-300) < 1e-12, "分块卷积 == 直接卷积", buf);
        }
        // 零延迟验证：delta 卷积应直接还原 IR 本身
        // 注意：必须喂足够多的块把整个 IR 长度都走出来（曾只喂了 2048 个样本
        // 却去比对 32768 个 IR 样本，越界读导致 ACCESS_VIOLATION）
        const size_t BB = 512;
        const size_t nBlocks = (irLen + BB - 1) / BB;
        PartitionedConvolver pc(ir, BB);
        std::vector<double> blk(BB, 0.0), oblk(BB), got;
        got.reserve(nBlocks * BB);
        for (size_t b = 0; b < nBlocks; ++b) {
            std::fill(blk.begin(), blk.end(), 0.0);
            if (b == 0) blk[0] = 1.0;                 // 输入为 delta
            pc.process_block(blk.data(), oblk.data());
            for (size_t i = 0; i < BB; ++i) got.push_back(oblk[i]);
        }
        double md = 0.0;
        for (size_t i = 0; i < irLen; ++i) md = std::max(md, std::fabs(got[i] - ir[i]));
        char buf[128];
        std::snprintf(buf, sizeof(buf), "(最大偏差 %.3e)", md);
        check(md < 1e-12, "零延迟：delta 卷积直接还原 IR", buf);
    }

    printf("\n=== 2. 双二阶 EQ：解析响应 vs 实测响应 ===\n");
    {
        const double fs = 48000.0;
        struct Case { const char* name; Biquad q; };
        std::vector<Case> cases;
        Biquad b;
        biquad_design::peak(b, fs, 1000.0, 1.0, 6.0);   cases.push_back({"peak 1k Q1 +6dB", b});
        biquad_design::peak(b, fs, 1000.0, 4.0, -9.0);  cases.push_back({"peak 1k Q4 -9dB", b});
        biquad_design::lowShelf(b, fs, 200.0, 0.7, 5.0);cases.push_back({"lshelf 200 +5dB", b});
        biquad_design::highShelf(b, fs, 6000.0, 0.7, -4.0); cases.push_back({"hshelf 6k -4dB", b});
        biquad_design::lowpass(b, fs, 2000.0, 0.707);   cases.push_back({"lp 2k Q0.707", b});
        biquad_design::highpass(b, fs, 100.0, 0.707);   cases.push_back({"hp 100 Q0.707", b});
        biquad_design::notch(b, fs, 50.0, 4.0);         cases.push_back({"notch 50 Q4", b});
        biquad_design::allpass(b, fs, 1000.0, 0.707);   cases.push_back({"ap 1k", b});
        biquad_design::lowpass1(b, fs, 1000.0);         cases.push_back({"lp1 1k", b});
        biquad_design::highpass1(b, fs, 1000.0);        cases.push_back({"hp1 1k", b});

        for (auto& c : cases) {
            double worst = 0.0;
            for (int i = 1; i <= 100; ++i) {
                const double f = 20.0 * std::pow(1000.0, (double)i / 100.0);   // 20 Hz ~ 20 kHz 对数扫
                if (f >= fs / 2.0) break;
                const double a = c.q.response_db(f / (fs / 2.0));
                const double m = c.q.measured_response_db(f / (fs / 2.0), 32768);
                if (std::fabs(a) < -200.0 && std::fabs(m) < -200.0) continue;
                worst = std::max(worst, std::fabs(a - m));
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "(解析 vs 实测最大偏差 %.4f dB)", worst);
            check(worst < 0.01, std::string(c.name) + " 解析式与实测一致", buf);
        }

        // peak 滤波器：f0 处增益必须等于设定值
        Biquad p;
        biquad_design::peak(p, fs, 1000.0, 2.0, 6.0);
        const double at = p.response_db(1000.0 / (fs / 2.0));
        char buf[128];
        std::snprintf(buf, sizeof(buf), "(f0 处 %.4f dB, 设定 6.0000 dB)", at);
        check(std::fabs(at - 6.0) < 0.01, "peak：f0 处增益等于设定值", buf);
    }

    printf("\n=== 3. 延迟插件 ===\n");
    {
        const double fs = 48000.0;
        DelayLine d;
        d.set_samples(137);
        std::vector<double> y(400, 0.0);
        for (size_t i = 0; i < y.size(); ++i) y[i] = d.process(i == 0 ? 1.0 : 0.0);
        size_t peak = 0; double best = -1.0;
        for (size_t i = 0; i < y.size(); ++i) if (std::fabs(y[i]) > best) { best = std::fabs(y[i]); peak = i; }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "(脉冲出现在第 %zu 个样本)", peak);
        check(peak == 137 && std::fabs(best - 1.0) < 1e-15, "样本延迟精确", buf);

        DelayLine d2;
        d2.set_meters(1.0, fs);      // 1 米 -> 1/343.956 秒
        const size_t expect = (size_t)std::llround(fs / 343.956);
        std::snprintf(buf, sizeof(buf), "(1 米 = %zu 样本, 期望 %zu)", d2.samples(), expect);
        check(std::abs((long long)d2.samples() - (long long)expect) <= 1, "距离延迟换算正确", buf);
    }

    printf("\n=== 4. Matrix 路由与增益 ===\n");
    {
        MatrixMixer mx;
        // 立体声 -> 立体声：L->L 0dB, R->R 0dB, L->R -6dB, R->L -6dB（串扰）
        std::vector<MatrixRoute> routes = {
            {0, 0, db_to_linear(0.0)},
            {1, 1, db_to_linear(0.0)},
            {0, 1, db_to_linear(-6.0)},
            {1, 0, db_to_linear(-6.0)},
        };
        mx.configure(2, 2, routes);

        const size_t N = 1000;
        std::vector<double> L(N, 0.0), R(N, 0.0);
        for (size_t i = 0; i < N; ++i) { L[i] = 0.5; R[i] = -0.25; }
        const double* in[2] = { L.data(), R.data() };
        std::vector<double> OL(N), OR(N);
        double* out[2] = { OL.data(), OR.data() };
        mx.process(in, out, N);

        const double g6 = db_to_linear(-6.0);
        const double wantL = 0.5 + g6 * (-0.25);
        const double wantR = -0.25 + g6 * 0.5;
        bool ok = true;
        for (size_t i = 0; i < N; ++i)
            if (std::fabs(OL[i] - wantL) > 1e-15 || std::fabs(OR[i] - wantR) > 1e-15) { ok = false; break; }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "(L=%.6f 期望 %.6f, R=%.6f 期望 %.6f, -6dB=%.6f)",
                      OL[0], wantL, OR[0], wantR, g6);
        check(ok, "串扰混合与 dB 增益精确", buf);

        // 反相路由（M/S 处理用）
        MatrixMixer mx2;
        mx2.configure(1, 1, { {0, 0, -1.0} });
        std::vector<double> a(N, 0.75), o(N);
        const double* i2[1] = { a.data() };
        double* o2[1] = { o.data() };
        mx2.process(i2, o2, N);
        check(std::fabs(o[0] + 0.75) < 1e-15, "负增益实现反相");
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
