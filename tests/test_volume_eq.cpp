// test_volume_eq.cpp — 音量控制与 REW/AutoEq 参数 EQ 解析验证（全程离线）
// 判据：
//   音量：0 dB 必须逐位一致；−6.0206 dB 必须恰好 ×0.5；渐变必须无跳变
//   EQ 文本：解析出的 Fc/Gain/Q 必须与原文一致；PK 在 fc 处增益必须等于设定值
#include "dsp/volume.h"
#include "dsp/eq_text.h"
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace rta::dsp;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== 1. 音量控制 ===\n");
    {
        VolumeControl v;
        v.set_sample_rate(48000.0);
        v.set_ramp_ms(0.0);          // 测试用瞬时切换
        v.set_gain_db(0.0);
        v.snap();

        // 0 dB 必须逐位一致（这是"比特完美"的一部分）
        std::vector<double> x(1000);
        for (size_t i = 0; i < 1000; ++i) x[i] = std::sin(0.01 * (double)i) * 0.7;
        std::vector<double> y = x;
        v.process(y.data(), y.size());
        bool identical = true;
        for (size_t i = 0; i < x.size(); ++i) if (x[i] != y[i]) { identical = false; break; }
        check(identical, "0 dB 逐位一致");

        // −6.0206 dB（= 0.5 倍）必须精确
        v.set_gain_db(20.0 * std::log10(0.5));
        v.snap();
        y = x;
        v.process(y.data(), y.size());
        // 20*log10(0.5) 不是精确的十进制值，libm 的 pow 可能差 1 ulp；
        // 不能用 == 判等（-ffast-math 曾把 pow 折叠成精确 0.5，掩盖了这一点）。
        bool half = true;
        for (size_t i = 0; i < x.size(); ++i)
            if (std::fabs(y[i] - x[i] * 0.5) > 1e-15) { half = false; break; }
        check(half, "-6.0206 dB = x0.5（1 ulp 内）");

        // 静音
        v.set_gain_db(-144.0);
        v.snap();
        y = x;
        v.process(y.data(), y.size());
        bool silent = true;
        for (double t : y) if (t != 0.0) { silent = false; break; }
        check(silent, "-144 dB 完全静音");

        // 渐变：必须单调、无跳变、终值正确
        VolumeControl r;
        r.set_sample_rate(48000.0);
        r.set_ramp_ms(100.0);        // 4800 样本
        r.set_gain_db(0.0); r.snap();
        r.set_gain_db(-20.0);
        const size_t N = 6000;
        std::vector<double> g(N);
        for (size_t i = 0; i < N; ++i) g[i] = r.process(1.0);
        bool mono = true; double maxJump = 0.0;
        for (size_t i = 1; i < N; ++i) {
            if (g[i] > g[i - 1] + 1e-15) mono = false;
            maxJump = std::max(maxJump, std::fabs(g[i] - g[i - 1]));
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf), "(终值 %.9f 期望 0.1, 最大单样本跳变 %.3e, 单调 %s)",
                      g[N - 1], maxJump, mono ? "是" : "否");
        check(mono && std::fabs(g[N - 1] - 0.1) < 1e-12 && maxJump < 1e-3, "渐变无跳变且终值正确", buf);

        // 手册 4.2 固定音量两档
        std::snprintf(buf, sizeof(buf), "(灰显 %.1f dB, 勾选 %.1f dB)",
                      VolumeControl::fixed_volume_grayed_db(), VolumeControl::fixed_volume_checked_db());
        check(VolumeControl::fixed_volume_grayed_db() == -3.0 &&
              VolumeControl::fixed_volume_checked_db() == -6.0, "固定音量两档符合手册", buf);
    }


    printf("\n=== 1b. 起播增益与多声道渐变（两轮审查 F01）===\n");
    {
        // ① 起播：−60 / −12 / 0 dB 的首块必须直接等于请求增益，不允许"单位增益前缀"。
        //    判据用【管线真实入口】prepare_stream()，不是手工 snap() —— 旧 test 里
        //    v.snap() 是测试自己补的，正好绕过了生产初始化缺 snap 的缺陷。
        struct Case { double db; double want; };
        const Case cases[] = { {-60.0, std::pow(10.0, -60.0 / 20.0)},
                               {-12.0, std::pow(10.0, -12.0 / 20.0)},
                               {  0.0, 1.0} };
        for (const auto& cs : cases) {
            VolumeControl v;
            v.set_sample_rate(48000.0);
            v.set_ramp_ms(200.0);
            v.set_gain_db_snapped(cs.db);        // 生产的起播语义
            const double g0 = v.process(1.0);    // 首个样本用的增益
            char b2[220];
            std::snprintf(b2, sizeof(b2), "(%+.0f dB: 首样本增益 %.9f, 目标 %.9f, 差 %.4f dB)",
                          cs.db, g0, cs.want,
                          (g0 > 0.0 && cs.want > 0.0) ? 20.0 * std::log10(g0 / cs.want) : -999.0);
            const bool ok = (cs.db == 0.0) ? (g0 == 1.0)
                                           : (std::fabs(20.0 * std::log10(g0 / cs.want)) < 1e-9);
            check(ok, "起播首样本 = 请求增益", b2);
        }

        // ② 同一帧左右声道必须用同一个增益（旧实现右声道拿到的是渐变后半段）
        {
            VolumeControl v;
            v.set_sample_rate(48000.0);
            v.set_ramp_ms(100.0);
            v.set_gain_db_snapped(0.0);
            v.set_gain_db(-20.0);                // 动态变化 ⟹ 启动渐变
            const size_t N = 500;
            std::vector<double> L(N, 1.0), R(N, 1.0);
            double* pl[2] = { L.data(), R.data() };
            v.process_planes(pl, 2, N);
            double maxDiff = 0.0;
            for (size_t i = 0; i < N; ++i) maxDiff = std::max(maxDiff, std::fabs(L[i] - R[i]));
            char b2[160];
            std::snprintf(b2, sizeof(b2), "(500 帧最大左右差 %.3e)", maxDiff);
            check(maxDiff == 0.0, "同一帧左右增益相同", b2);
        }

        // ③ 单/双/6 声道的 200 ms 渐变墙钟长度必须一致（按帧推进，不按样本推进）
        {
            std::vector<size_t> lens;
            for (int ch : {1, 2, 6}) {
                VolumeControl v;
                v.set_sample_rate(48000.0);
                v.set_ramp_ms(200.0);
                v.set_gain_db_snapped(0.0);
                v.set_gain_db(-20.0);
                size_t n = 0;
                while (v.ramping() && n < 100000) { v.step_frame(); ++n; }
                lens.push_back(n);
            }
            char b2[200];
            std::snprintf(b2, sizeof(b2), "(1ch=%zu 2ch=%zu 6ch=%zu 帧, 期望 9600)",
                          lens[0], lens[1], lens[2]);
            check(lens[0] == 9600 && lens[1] == 9600 && lens[2] == 9600,
                  "渐变墙钟长度与声道数无关", b2);
        }

        // ④ 4096 / 777 分块必须得到同一序列（渐变跨调用连续、与分块边界无关）
        {
            auto run = [](size_t block, std::vector<double>& out) {
                VolumeControl v;
                v.set_sample_rate(48000.0);
                v.set_ramp_ms(50.0);
                v.set_gain_db_snapped(0.0);
                v.set_gain_db(-30.0);
                const size_t N = 5000;
                std::vector<double> buf(N, 1.0);
                out.clear(); out.reserve(N);
                for (size_t p = 0; p < N; p += block) {
                    const size_t n = std::min(block, N - p);
                    double* pl[1] = { buf.data() + p };
                    v.process_planes(pl, 1, n);
                }
                out = buf;
            };
            std::vector<double> a, b2v;
            run(4096, a); run(777, b2v);
            bool same = (a.size() == b2v.size());
            if (same) for (size_t i = 0; i < a.size(); ++i) if (a[i] != b2v[i]) { same = false; break; }
            check(same, "4096 与 777 分块得到同一序列");
        }

        // ⑤ 0 dB 稳态必须逐位不变（含 −0.0 与极大/极小值）
        {
            VolumeControl v;
            v.set_sample_rate(48000.0);
            v.set_gain_db_snapped(0.0);
            std::vector<double> x = { 0.0, -0.0, 1.0, -1.0, 1e-300, -1e300, 0.1 };
            std::vector<double> y = x;
            double* pl[1] = { y.data() };
            v.process_planes(pl, 1, y.size());
            bool identical = true;
            for (size_t i = 0; i < x.size(); ++i)
                if (std::memcmp(&x[i], &y[i], sizeof(double)) != 0) { identical = false; break; }
            check(identical, "0 dB 稳态逐位不变（含 −0.0）");
        }
    }

    printf("\n=== 2. REW / AutoEq 参数 EQ 文本解析 ===\n");
    {
        const std::string rew = R"(Filter  1: ON PK     Fc   105.0 Hz  Gain   5.5 dB  Q  0.700
Filter  2: ON LS     Fc    80.0 Hz  Gain   2.0 dB  Q  0.700
Filter  3: ON HS     Fc  6000.0 Hz  Gain  -4.0 dB  Q  0.700
Filter  4: OFF PK    Fc   900.0 Hz  Gain   9.0 dB  Q  2.000
Filter  5: ON LP     Fc  1800.0 Hz  Q  0.707
)";
        EqSpec spec = parse_eq_text(rew);
        char buf[220];
        std::snprintf(buf, sizeof(buf), "(解析出 %zu 个滤波器)", spec.bands.size());
        check(spec.bands.size() == 5, "REW：滤波器个数正确", buf);

        if (spec.bands.size() == 5) {
            const auto& b0 = spec.bands[0];
            std::snprintf(buf, sizeof(buf), "(type=%s fc=%.1f gain=%.1f q=%.3f)",
                          b0.type.c_str(), b0.fc, b0.gain, b0.q);
            check(b0.type == "PK" && std::fabs(b0.fc - 105.0) < 1e-9 &&
                  std::fabs(b0.gain - 5.5) < 1e-9 && std::fabs(b0.q - 0.7) < 1e-9,
                  "第 1 条参数解析正确", buf);
            check(spec.bands[1].type == "LS" && std::fabs(spec.bands[1].fc - 80.0) < 1e-9, "LS 类型识别");
            check(spec.bands[2].type == "HS" && std::fabs(spec.bands[2].gain + 4.0) < 1e-9, "HS 类型与负增益");
            check(!spec.bands[3].enabled, "OFF 被识别为禁用");
            check(spec.bands[4].type == "LP", "LP 类型识别");
        }

        const std::string autoeq = R"(Preamp: -6.0 dB
Filter 1: ON PK Fc 105 Hz Gain 5.5 dB Q 0.70
Filter 2: ON PK Fc 3000 Hz Gain -3.0 dB Q 1.41
)";
        EqSpec a = parse_eq_text(autoeq);
        std::snprintf(buf, sizeof(buf), "(preamp=%.1f dB, %zu 个滤波器)", a.preampDb, a.bands.size());
        check(std::fabs(a.preampDb + 6.0) < 1e-9 && a.bands.size() == 2, "AutoEq：Preamp 与波段解析", buf);

        // 构建滤波链并验证响应
        const double fs = 48000.0;
        EqSpec one;
        EqBand pk; pk.type = "PK"; pk.fc = 1000.0; pk.gain = 6.0; pk.q = 2.0; pk.enabled = true;
        one.bands.push_back(pk);
        auto chain = build_eq_filters(one, fs);
        const double at = eq_chain_response_db(chain, 1000.0 / (fs / 2.0));
        std::snprintf(buf, sizeof(buf), "(1000 Hz 处 %.4f dB, 设定 6.0000 dB)", at);
        check(chain.size() == 1 && std::fabs(at - 6.0) < 0.01, "PK 在 fc 处增益等于设定值", buf);

        // 禁用波段必须被排除
        EqSpec two = one;
        two.bands[0].enabled = false;
        check(build_eq_filters(two, fs).empty(), "禁用的波段不进入滤波链");

        // 多段叠加：两段 PK 在各自频率上的增益应互不干扰（远离时）
        EqSpec ms;
        EqBand a1; a1.type = "PK"; a1.fc = 100.0;  a1.gain = 4.0; a1.q = 1.0; a1.enabled = true;
        EqBand a2; a2.type = "PK"; a2.fc = 8000.0; a2.gain = -5.0; a2.q = 1.0; a2.enabled = true;
        ms.bands = {a1, a2};
        auto mc = build_eq_filters(ms, fs);
        const double r1 = eq_chain_response_db(mc, 100.0 / (fs / 2.0));
        const double r2 = eq_chain_response_db(mc, 8000.0 / (fs / 2.0));
        const double mid = eq_chain_response_db(mc, 1000.0 / (fs / 2.0));
        std::snprintf(buf, sizeof(buf), "(100Hz %.2f, 8kHz %.2f, 1kHz %.2f dB)", r1, r2, mid);
        check(std::fabs(r1 - 4.0) < 0.05 && std::fabs(r2 + 5.0) < 0.05 && std::fabs(mid) < 0.5,
              "多段 EQ 叠加正确且互不干扰", buf);
    }

    printf("\n%s (失败项 %d)\n", fails == 0 ? "全部通过" : "存在失败", fails);
    return fails == 0 ? 0 : 1;
}
