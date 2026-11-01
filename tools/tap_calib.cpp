// tap_calib.cpp — 抽头数标定扫描（261030-01 规格 v2.1）
//
// 为什么需要它：
//   preset_metrics 原来用「Tref=160 @ Δf=4100」按 T ∝ 1/Δf 外推，但那个 Tref 是在
//   cutoff = pass（截止点压在通带边缘）下测的 —— 那个配置通带本身就塌（E-8 实测 T=74 时
//   0.95×pass 处 −1.91 dB）。规格 §4.5 要求 cutoff = pass + Δf/2，正确截止点下需要更多抽头。
//   而「在单一 Δf 上标一个常数再外推」被 E-8 证伪（Δf=8820 时 T=96 够，Δf=4410 时 T=191 不够）。
//
// 所以这里做真正的标定：对每个 (窗形, Δf) 扫 T，找「通带平直 且 镜像达家族天花板」的最小 T。
// 输出 T×Δf 乘积 —— 若它随 Δf 变化，就说明不能用一个常数，必须查表。
#include "dsp/filter_families.h"
#include "dsp/filter_design.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

using namespace rta::dsp;

static const double kFsIn  = 44100.0;
static const int    kL     = 16;          // 44.1k -> 705.6k
static const double kFsOut = kFsIn * kL;
static const double kMaxN  = 2000000.0;

// 设计一条原型并返回 {镜像处dB, 通带0.95pass处dB}
static void measure(WindowKind w, double df, int T, double ceiling, double& imgDb, double& passDb) {
    const double pass = (kFsIn - df) * 0.5;
    const double cut  = pass + df * 0.5;              // ★ 截止点 = 过渡带中点
    FirSpec s;
    s.taps    = T * kL;
    s.cutoff  = (cut / (kFsIn * 0.5)) / (double)kL;
    s.attenDb = ceiling;
    s.window  = w;
    auto h = design_fir(s);
    imgDb  = magnitude_db(h, (kFsIn - pass) / (kFsIn * kL * 0.5));
    passDb = magnitude_db(h, (pass * 0.95) / (kFsIn * kL * 0.5));
}

static const char* wname(WindowKind w) {
    return w == WindowKind::Kaiser ? "Kaiser"
         : w == WindowKind::BlackmanHarris ? "BH"
         : w == WindowKind::Gaussian ? "Gauss" : "?";
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("=== tap_calib: 每 (窗形, Δf) 求最小 T ===\n");
    printf("    判据：镜像 <= -ceiling dB  且  0.95*pass >= -0.01 dB\n");
    printf("    fsIn=%.0f  L=%d  cutoff=pass+df/2\n\n", kFsIn, kL);

    const double dfs[] = { 132.0, 353.0, 662.0, 882.0, 1323.0, 1764.0, 2646.0, 4410.0, 8820.0 };
    const WindowKind wins[] = { WindowKind::Kaiser, WindowKind::BlackmanHarris, WindowKind::Gaussian };

    printf("  %-8s %8s %10s %8s %10s %10s %12s\n",
           "window", "df(Hz)", "pass(Hz)", "T_min", "img(dB)", "pass(dB)", "T*t");

    for (WindowKind w : wins) {
        const double ceiling = preset_ceiling_db(w);   // 单一真相源（高斯=160，其余=280）
        for (double df : dfs) {
            const double pass = (kFsIn - df) * 0.5;
            if (pass <= 1000.0) continue;
            int bestT = -1; double bestImg = 0, bestPass = 0;
            // 从 8 到 8192 按 1.06 倍几何扫描（约 120 个点，够细）
            for (double Td = 8.0; Td <= 8192.0; Td *= 1.06) {
                const int T = (int)(Td + 0.5);
                if ((double)T * kL > kMaxN) break;
                double img = 0, ps = 0;
                measure(w, df, T, ceiling, img, ps);
                if (img <= -ceiling && ps >= -0.01) { bestT = T; bestImg = img; bestPass = ps; break; }
            }
            if (bestT < 0) {
                printf("  %-8s %8.0f %10.0f %8s %10s %10s %14s\n", wname(w), df, pass, "FAIL", "-", "-", "-");
            } else {
                printf("  %-8s %8.0f %10.0f %8d %10.1f %10.4f %12.2f\n",
                       wname(w), df, pass, bestT, bestImg, bestPass, (double)bestT * df / kFsIn);
            }
        }
        printf("\n");
    }

    // 回归断言：E-8 的已知点（Kaiser, df=8820, ceiling=280 ⟹ T 应约为 96）
    {
        double img = 0, ps = 0;
        measure(WindowKind::Kaiser, 8820.0, 96, 280.0, img, ps);
        printf("回归断言 Kaiser df=8820 T=96: img=%.1f dB pass=%.4f dB  %s\n",
               img, ps, (img <= -280.0 && ps >= -0.01) ? "PASS" : "FAIL");
        return (img <= -280.0 && ps >= -0.01) ? 0 : 1;
    }
}
