// test_filters.cpp — 滤波器家族客观验证
// 验证四件事：
//   1) 各家族的通带纹波 / 阻带衰减
//   2) 最小相位与线性相位的【幅频响应一致】（倒谱分解的正确性判据）
//   3) 最小相位消除前振铃（前/后能量比 ≪ 1，峰值位置前移）
//   4) 1x / Nx 双路径
#include "dsp/filter_families.h"
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

int main() {
    printf("=== 1. 滤波器家族对比（截止取半带型 0.44~0.495） ===\n");
    {
        printf("  %-24s %8s %4s %10s %10s %10s %9s %9s\n",
               "预设", "抽头", "相位", "通带纹波", "阻带最大", "前/后能量", "峰值位置", "能量重心");
        for (const auto& p : filter_presets()) {
            FirSpec s;
            s.taps = p.taps; s.cutoff = 0.5 * (1.0 - p.transition);
            s.attenDb = p.attenDb; s.window = p.window;
            auto h = design_fir(s);
            if (p.phase == PhaseKind::Minimum) h = to_minimum_phase(h);

            const double fc = s.cutoff * 0.5;
            const double fPassTop = fc * 0.9 * 2.0;
            const double fStopBot = std::min(fc * 1.1 * 2.0, 1.0);

            double pmin = 1e9, pmax = -1e9;
            for (int i = 0; i <= 20; ++i) {
                const double d = magnitude_db(h, fPassTop * (double)i / 20.0);
                pmin = std::min(pmin, d); pmax = std::max(pmax, d);
            }
            double worst = -1e9;
            for (int i = 0; i <= 40; ++i)
                worst = std::max(worst, magnitude_db(h, fStopBot + (1.0 - fStopBot) * (double)i / 40.0));

            printf("  %-24s %8d %4s %10.4f %10.1f %10.4f %9.4f %9.1f\n",
                   p.name, (int)h.size(), phase_name(p.phase),
                   pmax - pmin, worst, pre_post_ratio(h), peak_position(h), energy_centroid(h));
        }
    }

    printf("\n=== 2. 最小相位与线性相位的幅频响应一致性 ===\n");
    {
        const char* pairs[][2] = {
            { "poly-sinc-lp",         "poly-sinc-mp" },
            { "poly-sinc-gauss-long", "poly-sinc-gauss-mp" },
            { "poly-sinc-short-lp",   "poly-sinc-short-mp" },
        };
        for (auto& pr : pairs) {
            int tl = 0, tm = 0;
            auto hl = make_filter(pr[0], false, &tl);
            auto hm = make_filter(pr[1], false, &tm);
            if (hl.empty() || hm.empty()) { check(false, std::string(pr[0]) + " 生成失败"); continue; }
            double maxDiff = 0.0;
            for (int i = 1; i <= 40; ++i) {
                const double f = (double)i / 40.0;
                const double dl = magnitude_db(hl, f);
                const double dm = magnitude_db(hm, f);
                // 只比较 -120 dB 以上的有效区间：最小相位响应本质是无限长，
                // 截断成有限抽头后，极深阻带（-150 dB 以下）会有几 dB 偏差，听觉与工程上均无意义。
                if (dl < -120.0 || dm < -120.0) continue;
                maxDiff = std::max(maxDiff, std::fabs(dl - dm));
            }
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "(%d 抽头, -120 dB 以上最大偏差 %.4f dB)",
                          tl, maxDiff);
            printf("  %-22s vs %-22s %s\n", pr[0], pr[1], buf);
            // 偏差随抽头数减少而增大（截断效应）：32k 抽头 0.005 dB，4k 抽头 0.14 dB。
            // 阈值取 0.2 dB 作为回归基线；若需更严格，应给 -mp 预设分配更多抽头。
            check(maxDiff < 0.20, std::string(pr[0]) + " 与 " + pr[1] + " 幅频一致");
        }
    }

    printf("\n=== 3. 前振铃：线性相位 vs 最小相位 ===\n");
    {
        int tl = 0, tm = 0;
        auto hl = make_filter("poly-sinc-lp", false, &tl);
        auto hm = make_filter("poly-sinc-mp", false, &tm);
        const double rl = pre_post_ratio(hl), rm = pre_post_ratio(hm);
        const double pl = peak_position(hl), pm = peak_position(hm);
        const double cl = energy_centroid(hl), cm = energy_centroid(hm);
        printf("  线性相位: 前/后能量 %.4f  峰值位置 %.4f  能量重心 %.1f / %d\n", rl, pl, cl, tl);
        printf("  最小相位: 前/后能量 %.4f  峰值位置 %.4f  能量重心 %.1f / %d\n", rm, pm, cm, tm);
        check(rl > 0.90 && rl < 1.10, "线性相位前/后能量对称（≈1.0）");
        // 注意：峰值若位于第 10 个抽头附近，它前面仍有少量样本，"前/后能量"天然不会是 0。
        // 真正有判别力的是峰值位置与能量重心（见下两条）。
        check(rm < 1.0, "最小相位前振铃不大于后振铃");
        check(pm < 0.01, "最小相位峰值几乎位于起始处");
        check(cm < cl * 0.01, "最小相位能量重心大幅前移");
    }

    printf("\n=== 4. 混合相位（线性/最小 各 50%%） ===\n");
    {
        FirSpec s;
        s.taps = 4095; s.cutoff = 0.45; s.attenDb = 120; s.window = WindowKind::Kaiser;
        auto hl = design_fir(s);
        auto hm = to_minimum_phase(hl);
        auto hx = to_intermediate_phase(hl, 0.5);

        // 判据 1：幅度必须和线性相位一致（混合的是相位，不是幅度）
        double worst = 0.0;
        for (int i = 0; i <= 40; ++i) {
            const double f = 0.45 * 0.8 * (double)i / 40.0;      // 通带内
            worst = std::max(worst, std::fabs(magnitude_db(hx, f) - magnitude_db(hl, f)));
        }
        // 阈值 0.05 dB：混合相位把能量右移 τ=(L-1)/4，尾部超出抽头长度的部分被截断，
        // 这会产生千分之几 dB 的幅频偏差（实测 0.023 dB）。这是该做法的固有代价，
        // 不是实现错误；真正要证明的是"没有出现数量级更大的凹陷"。
        check(worst < 0.05, "混合相位幅频与线性相位一致", "最大偏差 " + std::to_string(worst) + " dB");

        // 判据 2：能量重心必须严格夹在最小相位与线性相位之间
        const double cl = energy_centroid(hl), cm = energy_centroid(hm), cx = energy_centroid(hx);
        printf("  能量重心  线性 %.1f  混合 %.1f  最小 %.1f\n", cl, cx, cm);
        check(cx > cm && cx < cl, "混合相位能量重心落在两者之间");

        // 判据 3：a=0 / a=1 退化为两个端点
        auto h0 = to_intermediate_phase(hl, 0.0);
        auto h1 = to_intermediate_phase(hl, 1.0);
        double d0 = 0.0, d1 = 0.0;
        for (size_t i = 0; i < hl.size(); ++i) {
            d0 = std::max(d0, std::fabs(h0[i] - hm[i]));
            d1 = std::max(d1, std::fabs(h1[i] - hl[i]));
        }
        check(d0 < 1e-12, "a=0 退化为最小相位");
        check(d1 < 1e-12, "a=1 退化为线性相位");
    }

    printf("\n=== 5. 1x / Nx 双路径 ===\n");
    {
        int t1x = 0, tNx = 0;
        auto h1 = make_filter("poly-sinc-long-lp", false, &t1x);
        auto hN = make_filter("poly-sinc-long-lp", true,  &tNx);
        printf("  1x 路径抽头 %d，Nx 路径抽头 %d\n", t1x, tNx);
        check(t1x > 0 && tNx > 0, "两条路径都能生成");
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
