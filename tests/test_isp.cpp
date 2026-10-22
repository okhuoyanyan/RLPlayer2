// test_isp.cpp — 过采样峰值（ISP）检测的正确性验证
//
// 判据设计（铁律 3：测试通过 ≠ 测对了东西）：
//   光看"没崩"毫无意义，必须用一个【已知答案】的构造信号：
//   fs/4 的正弦、相位 45°，采样点只有 ±0.7071，但两个采样点之间的真实波形峰值是 1.0。
//   检测器必须报出 ≈1.0（而不是 0.707）—— 这才证明它真的在插值，而不是在看采样值。
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "dsp/oversample_peak.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

using namespace rta::dsp;

static int g_fail = 0;
static void ck(bool ok, const char* what, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what,
                detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++g_fail;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::printf("== 过采样峰值检测验证 ==\n");

    // 1) 插值核自身
    const auto& K = isp_kernel();
    ck(K.size() == 4, "4 个相位", std::to_string(K.size()));
    {
        double s = 0.0;
        for (double v : K[0]) s += v;
        ck(std::fabs(s - 1.0) < 1e-12, "相 0 直流增益 = 1", std::to_string(s));
        int nz = 0, mid = -1;
        for (int i = 0; i < (int)K[0].size(); ++i) if (std::fabs(K[0][(size_t)i]) > 1e-12) { ++nz; mid = i; }
        ck(nz == 1 && mid == 3, "相 0 退化为 delta（不引入偏差）", "非零点=" + std::to_string(nz));
    }
    for (int p = 1; p < 4; ++p) {
        double s = 0.0;
        for (double v : K[(size_t)p]) s += v;
        ck(std::fabs(s - 1.0) < 1e-9, ("相 " + std::to_string(p) + " 直流增益 = 1").c_str(), std::to_string(s));
    }

    // 2) 已知答案：fs/4 正弦 + 45° 相位
    {
        const int N = 400;
        std::vector<double> x((size_t)N);
        for (int n = 0; n < N; ++n)
            x[(size_t)n] = std::sin(2.0 * 3.14159265358979323846 * (0.25 * n + 0.125));
        double smax = 0.0;
        for (double v : x) smax = std::max(smax, std::fabs(v));
        const auto rep = detect_isp({ x });
        ck(std::fabs(smax - 0.7071) < 0.001, "构造信号的采样点峰值 ≈ 0.7071", std::to_string(smax));
        ck(rep.peak > 0.95 && rep.peak < 1.05, "检测器报出插值峰值 ≈ 1.0（真的在插值）",
           std::to_string(rep.peak));
        ck(rep.scanned, "标记为已扫描");
    }

    // 3) 常量信号：不应有"过冲"
    {
        std::vector<double> x(200, 0.5);
        const auto rep = detect_isp({ x });
        ck(std::fabs(rep.peak - 0.5) < 1e-6, "常量 0.5 的峰值 = 0.5", std::to_string(rep.peak));
        ck(rep.overCount == 0, "常量信号没有超幅点");
    }

    // 4) 低电平正弦：不应误报超幅
    {
        const int N = 2000;
        std::vector<double> x((size_t)N);
        for (int n = 0; n < N; ++n) x[(size_t)n] = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * n / 44100.0);
        const auto rep = detect_isp({ x });
        ck(rep.peak <= 0.5 + 1e-3, "-6 dBFS 1 kHz 正弦不误报", std::to_string(rep.peak));
        ck(rep.overCount == 0, "无误报超幅点");
    }

    // 5) 满幅方波：插值必然有吉布斯过冲 -> 应被检测到
    {
        std::vector<double> x(1000);
        for (int n = 0; n < 1000; ++n) x[(size_t)n] = (n / 8) % 2 ? -1.0 : 1.0;
        const auto rep = detect_isp({ x });
        ck(rep.peak > 1.0, "满幅方波的插值过冲被检出", std::to_string(rep.peak));
        ck(rep.overCount > 0, "超幅点数 > 0", std::to_string(rep.overCount));
    }

    // 6) 多声道：取最大者
    {
        std::vector<std::vector<double>> pl(2);
        pl[0].assign(100, 0.3);
        pl[1].assign(100, 0.9);
        const auto rep = detect_isp(pl);
        ck(std::fabs(rep.peak - 0.9) < 1e-6, "多声道取最大", std::to_string(rep.peak));
        ck(rep.atCh == 1, "记录到正确声道", std::to_string(rep.atCh));
    }

    std::printf("\n%s\n", g_fail == 0 ? "全部通过" : (std::to_string(g_fail) + " 项失败").c_str());
    return g_fail == 0 ? 0 : 1;
}
