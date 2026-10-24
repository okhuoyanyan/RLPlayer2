// test_peq.cpp — 20 段参数均衡（PEQ）验证（全程离线，不打开任何音频设备）
// 261024-02 #6 判据：
//   1) 全段 0dB：逐样本直通一致（float 可精确表示的值）
//   2) 单段 +6dB：中心频率实测 6 ± 0.1 dB（正弦 RMS 实测 + 解析响应交叉验证）
//   3) Q 变化：-3dB 带宽随 Q 成反比变化
//   4) 采样率切换：44.1/48/96/192k 下中心频率增益一致，系数随采样率重建
//   5) 极值/过载后恢复（铁律 7）：满幅、单样本越界、瞬态后恢复，无 NaN/不爆
//   6) 热更新交叉淡入：参数突变不产生阶跃（对比"硬切"的理论阶跃）
//   7) bypass 直通；预设序列化 round-trip；内置预设
#include "dsp/peq.h"
#include "engine/pipeline.h"   // 集成判据：PEQ 挂到播放管线上（输入采样率、现有 EQ 之前）
#include <memory>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace rta::dsp;
using rta::audio::AudioBuffer;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}
static std::string num(double v, int prec = 4) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

// 生成正弦并送进引擎，返回稳态窗口的 RMS 增益（dB）
static double measured_gain_db(const PeqSettings& s, double fs, double freq,
                               size_t total = 96000, size_t skip = 24000) {
    PeqEngine e;
    e.prepare(fs, 1);
    e.set_settings(s);
    std::vector<double> buf(total);
    for (size_t i = 0; i < total; ++i)
        buf[i] = std::sin(2.0 * kPi * freq * (double)i / fs);
    e.process(0, buf.data(), buf.size());
    double in2 = 0.0, out2 = 0.0;
    size_t n = 0;
    for (size_t i = skip; i < total; ++i) {
        const double x = std::sin(2.0 * kPi * freq * (double)i / fs);
        in2 += x * x;
        out2 += buf[i] * buf[i];
        ++n;
    }
    return 10.0 * std::log10((out2 / (double)n) / (in2 / (double)n) + 1e-300);
}

static PeqSettings one_band(double freq, double gain, double q) {
    PeqSettings s = peq_flat_settings();
    for (auto& b : s.bands) b.enabled = false;
    s.bands[0].enabled = true;
    s.bands[0].type = PeqBandType::Peak;
    s.bands[0].freq = freq;
    s.bands[0].gainDb = gain;
    s.bands[0].q = q;
    return s;
}

// -3dB 带宽（相对峰值）：解析响应扫描，返回 [fLow, fHigh]
static void band3db(const PeqSettings& s, double fs, double fc, double& lo, double& hi) {
    const double peak = peq_response_db(s, fs, fc);
    const double target = peak - 3.0;
    // 从中心频率向两侧找「响应降到峰值-3dB」的交叉点
    lo = hi = fc;
    for (double f = fc; f > 20.0; f /= 1.01) {
        if (peq_response_db(s, fs, f) <= target) { lo = f; break; }
    }
    for (double f = fc * 1.01; f < fs * 0.49; f *= 1.01) {
        if (peq_response_db(s, fs, f) <= target) { hi = f; break; }
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== 1. 默认 20 段参数 ===\n");
    {
        const auto s = peq_flat_settings();
        check(s.bands.size() == 20, "默认 20 段", "n=" + num((double)s.bands.size(), 0));
        const auto freqs = peq_default_frequencies();
        check(freqs.size() == 20, "默认频率表 20 个");
        bool match = true;
        for (size_t i = 0; i < s.bands.size() && i < freqs.size(); ++i)
            if (std::fabs(s.bands[i].freq - freqs[i]) > 1e-9) match = false;
        check(match, "段频率与文档一致（20Hz~20kHz）");
        check(std::fabs(freqs.front() - 20.0) < 1e-9 && std::fabs(freqs.back() - 20000.0) < 1e-9,
              "覆盖 20Hz 与 20kHz");
        bool qok = true, gok = true, eok = true;
        for (const auto& b : s.bands) {
            if (std::fabs(b.q - 1.41) > 1e-9) qok = false;
            if (std::fabs(b.gainDb) > 1e-12) gok = false;
            if (!b.enabled) eok = false;
        }
        check(qok, "每段默认 Q=1.41");
        check(gok, "每段默认增益 0dB");
        check(eok, "每段默认启用");
        check(peq_builtin_preset_names().size() >= 2, "内置预设 >=2（Flat + 示例）");
    }

    printf("=== 2. 全段 0dB 直通逐样本一致 ===\n");
    {
        PeqEngine e;
        e.prepare(48000.0, 2);
        e.set_settings(peq_flat_settings());
        // 用 float 可精确表示的样本值，确保「float32 处理」后仍逐位一致
        std::vector<double> x = { 0.0, 0.5, -0.25, 0.125, 0.75, -0.5, 0.375, -1.0, 0.0625 };
        std::vector<double> y = x;
        e.process(0, y.data(), y.size());
        bool same = (x == y);
        check(same, "20 段全 0dB 逐样本一致");
        // 非零前级也要能直通判断：preamp=0 时链为空
        PeqSettings s2 = peq_flat_settings();
        s2.bypass = true;
        PeqEngine e2;
        e2.prepare(44100.0, 1);
        e2.set_settings(s2);
        std::vector<double> z = x;
        e2.process(0, z.data(), z.size());
        check(x == z, "bypass 逐样本一致");
    }

    printf("=== 3. 单段 +6dB 中心频率实测 ===\n");
    {
        const double fs = 48000.0;
        for (double fc : { 100.0, 1000.0, 5000.0, 10000.0 }) {
            PeqSettings s = one_band(fc, 6.0, 1.41);
            const double ana = peq_response_db(s, fs, fc);
            const double mea = measured_gain_db(s, fs, fc);
            check(std::fabs(ana - 6.0) <= 0.1, "解析响应 " + num(fc, 0) + "Hz = 6±0.1dB", num(ana));
            check(std::fabs(mea - 6.0) <= 0.1, "正弦实测 " + num(fc, 0) + "Hz = 6±0.1dB", num(mea));
        }
        // 增益符号：-6dB
        PeqSettings cut = one_band(1000.0, -6.0, 1.41);
        check(std::fabs(peq_response_db(cut, fs, 1000.0) + 6.0) <= 0.1, "单段 -6dB 中心实测 -6±0.1dB",
              num(peq_response_db(cut, fs, 1000.0)));
    }

    printf("=== 4. Q 变化与 -3dB 带宽 ===\n");
    {
        const double fs = 48000.0;
        PeqSettings wide = one_band(1000.0, 6.0, 0.7);
        PeqSettings narrow = one_band(1000.0, 6.0, 2.8);
        double lo1, hi1, lo2, hi2;
        band3db(wide, fs, 1000.0, lo1, hi1);
        band3db(narrow, fs, 1000.0, lo2, hi2);
        const double bw1 = hi1 - lo1, bw2 = hi2 - lo2;
        check(bw1 > bw2 * 1.5, "Q 越大 -3dB 带宽越窄",
              "Q0.7 BW=" + num(bw1, 1) + "Hz, Q2.8 BW=" + num(bw2, 1) + "Hz");
        const double ratio = bw1 / std::max(bw2, 1e-9);
        check(ratio > 2.5 && ratio < 6.0, "带宽比与 Q 比同量级（约 4x）", num(ratio, 2) + "x");
        // 实测：Q 变大后，中心频率仍为 +6dB
        check(std::fabs(measured_gain_db(narrow, fs, 1000.0) - 6.0) <= 0.1, "窄带 Q 中心仍 6dB");
        check(std::fabs(measured_gain_db(wide, fs, 1000.0) - 6.0) <= 0.1, "宽带 Q 中心仍 6dB");
    }

    printf("=== 5. 采样率相关（44.1/48/96/192k）===\n");
    {
        for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
            PeqSettings s = one_band(1000.0, 6.0, 1.41);
            const double ana = peq_response_db(s, fs, 1000.0);
            const double mea = measured_gain_db(s, fs, 1000.0);
            check(std::fabs(ana - 6.0) <= 0.1 && std::fabs(mea - 6.0) <= 0.1,
                  "fs=" + num(fs, 0) + "Hz 中心 6±0.1dB", "ana=" + num(ana) + " mea=" + num(mea));
            auto c = peq_design(s, fs);
            check(c.size() == 1 && std::fabs(c[0].b0 - 1.0) > 1e-6,
                  "fs=" + num(fs, 0) + "Hz 系数已按采样率重建", "b0=" + num(c[0].b0, 6));
        }
        // 采样率切换后引擎可用：prepare 新 fs 再测
        PeqEngine e;
        PeqSettings s = one_band(1000.0, 6.0, 1.41);
        e.prepare(44100.0, 1);
        e.set_settings(s);
        e.prepare(96000.0, 1);
        check(std::fabs(e.sample_rate() - 96000.0) < 1e-9, "切率后引擎采样率更新");
    }

    printf("=== 6. 极值 / 满幅 / 过载后恢复（铁律 7）===\n");
    {
        PeqSettings boost = peq_flat_settings();
        for (auto& b : boost.bands) { b.gainDb = 12.0; b.q = 1.41; }
        const double fs = 48000.0;
        PeqEngine e;
        e.prepare(fs, 1);
        e.set_settings(boost);
        // 满幅 0dBFS 正弦（1.0 峰值）
        const size_t n = (size_t)fs;   // 1s
        std::vector<double> buf(n);
        for (size_t i = 0; i < n; ++i) buf[i] = std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
        e.process(0, buf.data(), buf.size());
        bool finite = true, bounded = true;
        double peak = 0.0;
        for (double v : buf) {
            if (!std::isfinite(v)) finite = false;
            peak = std::max(peak, std::fabs(v));
            if (std::fabs(v) > 1e4) bounded = false;
        }
        check(finite, "全段 +12dB 满幅输入：输出无 NaN/Inf");
        check(bounded, "全段 +12dB 满幅输入：输出有界（不爆到发散）", "peak=" + num(peak, 2));
        // 单样本越界（1.5）+ 瞬态
        std::vector<double> imp(2048, 0.0);
        imp[0] = 1.5;
        imp[1] = -1.5;
        e.process(0, imp.data(), imp.size());
        bool impFinite = true;
        for (double v : imp) if (!std::isfinite(v)) impFinite = false;
        check(impFinite, "单样本 ±1.5 越界：无 NaN/Inf");
        // 恢复：静音输入，输出必须衰减回 0（不是锁死的满幅方波/直流）
        std::vector<double> tail((size_t)fs, 0.0);
        e.process(0, tail.data(), tail.size());
        double rms = 0.0, dc = 0.0;
        size_t cnt = 0;
        for (size_t i = tail.size() / 2; i < tail.size(); ++i) { rms += tail[i] * tail[i]; dc += tail[i]; ++cnt; }
        rms = std::sqrt(rms / (double)cnt);
        dc /= (double)cnt;
        check(rms < 1e-4, "瞬态后恢复：0.5s 后 RMS < -80dBFS", "rms=" + num(rms, 8));
        check(std::fabs(dc) < 1e-4, "瞬态后无直流锁死", "dc=" + num(dc, 8));
        // 全段 -12dB 衰减：满幅输入输出有限且被衰减
        PeqSettings cut = peq_flat_settings();
        for (auto& b : cut.bands) b.gainDb = -12.0;
        PeqEngine e2;
        e2.prepare(fs, 1);
        e2.set_settings(cut);
        std::vector<double> b2((size_t)fs);
        for (size_t i = 0; i < b2.size(); ++i) b2[i] = std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
        e2.process(0, b2.data(), b2.size());
        bool cutFinite = true;
        double cutPeak = 0.0;
        for (double v : b2) { if (!std::isfinite(v)) cutFinite = false; cutPeak = std::max(cutPeak, std::fabs(v)); }
        check(cutFinite && cutPeak < 1.0, "全段 -12dB 满幅输入：有限且被衰减", "peak=" + num(cutPeak, 4));
    }

    printf("=== 7. 热更新交叉淡入（避免爆音）===\n");
    {
        const double fs = 48000.0;
        PeqEngine e;
        e.prepare(fs, 1);
        e.set_settings(peq_flat_settings());
        // 用一整段相位连续的 1kHz 正弦，中途换参数；这样 delta 里只有"换参数"的影响，
        // 不会把两块缓冲的相位跳变误判成爆音。
        const size_t n = 16384;
        std::vector<double> buf(n);
        const double A = 0.5;
        for (size_t i = 0; i < n; ++i) buf[i] = A * std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
        e.process(0, buf.data(), 2048);          // 先跑一段稳定
        // 把 1kHz 提升 +6dB：若"硬切"，峰值处会有约 A 的阶跃
        e.set_settings(one_band(1000.0, 6.0, 1.41));
        e.process(0, buf.data() + 2048, n - 2048);
        double maxDelta = 0.0;
        for (size_t i = 2048; i < n; ++i) maxDelta = std::max(maxDelta, std::fabs(buf[i] - buf[i - 1]));
        // +6dB 后稳态正弦自身的每样本最大斜率 ≈ 2A*2π*1000/48000 ≈ 0.131；
        // 若硬切，峰值处还会叠加约 A=0.5 的阶跃（总 delta 可达 ~0.63）。
        // 判据：实测 maxΔ 只允许比稳态斜率多 15% 以内 ⟹ 说明没有阶跃。
        const double steadySlope = 2.0 * A * 2.0 * kPi * 1000.0 / fs;
        check(maxDelta < steadySlope * 1.15, "参数突变无阶跃（交叉淡入）",
              "maxΔ=" + num(maxDelta, 4) + " 稳态斜率≈" + num(steadySlope, 4));
        // 淡入完成后稳态增益仍应为 +6dB
        std::vector<double> tail(48000);
        for (size_t i = 0; i < tail.size(); ++i) tail[i] = std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
        e.process(0, tail.data(), tail.size());
        double in2 = 0.0, out2 = 0.0;
        size_t cnt = 0;
        for (size_t i = 24000; i < tail.size(); ++i) {
            const double x = std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
            in2 += x * x; out2 += tail[i] * tail[i]; ++cnt;
        }
        const double g = 10.0 * std::log10((out2 / cnt) / (in2 / cnt));
        check(std::fabs(g - 6.0) <= 0.1, "热更新后稳态增益 6±0.1dB", num(g));
    }

    printf("=== 8. 架式滤波器（LS/HS）===\n");
    {
        const double fs = 48000.0;
        PeqSettings ls = peq_flat_settings();
        for (auto& b : ls.bands) b.enabled = false;
        ls.bands[0].enabled = true; ls.bands[0].type = PeqBandType::LowShelf;
        ls.bands[0].freq = 100.0; ls.bands[0].gainDb = 6.0; ls.bands[0].q = 0.7;
        check(std::fabs(peq_response_db(ls, fs, 20.0) - 6.0) <= 0.6, "低架 20Hz ≈ +6dB", num(peq_response_db(ls, fs, 20.0)));
        check(std::fabs(peq_response_db(ls, fs, 10000.0)) <= 0.2, "低架 10kHz ≈ 0dB", num(peq_response_db(ls, fs, 10000.0)));
        PeqSettings hs = peq_flat_settings();
        for (auto& b : hs.bands) b.enabled = false;
        hs.bands[0].enabled = true; hs.bands[0].type = PeqBandType::HighShelf;
        hs.bands[0].freq = 8000.0; hs.bands[0].gainDb = -6.0; hs.bands[0].q = 0.7;
        check(std::fabs(peq_response_db(hs, fs, 18000.0) + 6.0) <= 0.8, "高架 18kHz ≈ -6dB", num(peq_response_db(hs, fs, 18000.0)));
        check(std::fabs(peq_response_db(hs, fs, 100.0)) <= 0.2, "高架 100Hz ≈ 0dB", num(peq_response_db(hs, fs, 100.0)));
    }

    printf("=== 9. 序列化 / 预设 round-trip ===\n");
    {
        PeqSettings s = peq_flat_settings();
        s.preampDb = -3.5;
        s.presetName = "测试预设";
        s.bypass = false;
        s.bands[3].gainDb = 4.5;
        s.bands[3].q = 2.0;
        s.bands[3].type = PeqBandType::LowShelf;
        s.bands[7].enabled = false;
        const std::string txt = peq_to_text(s);
        PeqSettings back;
        std::string err;
        check(peq_from_text(txt, back, &err), "序列化后可解析", err);
        check(back.bands.size() == s.bands.size(), "段数一致");
        check(std::fabs(back.preampDb + 3.5) < 1e-9, "preamp 保留");
        check(back.presetName == "测试预设", "预设名保留");
        check(std::fabs(back.bands[3].gainDb - 4.5) < 1e-9 && std::fabs(back.bands[3].q - 2.0) < 1e-9,
              "单段参数保留");
        check(back.bands[3].type == PeqBandType::LowShelf, "段类型保留（LS）");
        check(!back.bands[7].enabled, "段启用状态保留（off）");
        // 内置预设
        for (const std::string& name : peq_builtin_preset_names()) {
            PeqSettings p;
            check(peq_builtin_preset(name, p) && p.bands.size() == 20, "内置预设可加载：" + name);
        }
        PeqSettings harman;
        peq_builtin_preset("Harman 风格（示例）", harman);
        bool nonzero = false;
        for (const auto& b : harman.bands) if (std::fabs(b.gainDb) > 0.1) nonzero = true;
        check(nonzero, "示例预设含非零增益");
        check(std::fabs(harman.preampDb) > 0.1, "示例预设带前级衰减（防削波）");
    }


    printf("=== 10. 管线集成（Pipeline 挂载 PEQ，不打开设备）===\n");
    {
        const double fs = 48000.0;
        auto mk = [](const PeqSettings& s) {
            auto e = std::make_shared<PeqEngine>();
            e->prepare(48000.0, 2);
            e->set_settings(s);
            return e;
        };
        // 管线内部按整数样本工作（float 输入会被当整数缩放 ⟹ 全 0），
        // 这里用 24bit 整数样本，与真实播放路径一致。
        const double kScale = 8388607.0;
        auto run = [&](std::shared_ptr<PeqEngine> peq) {
            rta::engine::PipelineConfig cfg;
            cfg.peq = peq;
            cfg.outBits = 24;
            cfg.outRate = 48000;
            rta::engine::Pipeline pl(cfg);
            AudioBuffer in;
            in.rate = 48000; in.channels = 2; in.bits = 24; in.isFloat = false;
            const size_t n = 9600;
            in.raw.assign(n * 2, 0);
            // −20dBFS 测试音：+6dB 后仍远低于满幅，避免输出端 24bit 量化削波污染判据
            for (size_t i = 0; i < n; ++i) {
                const double v = 0.1 * std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
                in.setSample(i, 0, v); in.setSample(i, 1, v);
            }
            return pl.process(in, nullptr);
        };
        // 全 0dB：PEQ 不应改变任何样本
        auto eFlat = mk(peq_flat_settings());
        AudioBuffer oFlat = run(eFlat);
        AudioBuffer ref = run(nullptr);
        bool identical = (oFlat.raw.size() == ref.raw.size());
        if (identical)
            for (size_t i = 0; i < ref.raw.size(); ++i)
                if (oFlat.raw[i] != ref.raw[i]) { identical = false; break; }
        check(identical, "管线集成：PEQ 全 0dB 与无 PEQ 逐样本一致");

        // +6dB @1kHz：管线输出应实测约 +6dB
        auto eBoost = mk(one_band(1000.0, 6.0, 1.41));
        AudioBuffer oBoost = run(eBoost);
        double in2 = 0.0, out2 = 0.0;
        size_t cnt = 0;
        const size_t skip = 2400;
        for (size_t i = skip; i < oBoost.frames(); ++i) {
            const double x = 0.1 * std::sin(2.0 * kPi * 1000.0 * (double)i / fs);
            const double y = oBoost.sample(i, 0);
            in2 += x * x;
            out2 += y * y;
            ++cnt;
        }
        const double g = 10.0 * std::log10((out2 / (double)cnt) / (in2 / (double)cnt) + 1e-300);
        check(std::fabs(g - 6.0) <= 0.15, "管线集成：+6dB@1kHz 实测 6±0.15dB", num(g));
        rta::engine::PipelineConfig cf;
        cf.peq = mk(one_band(1000.0, 6.0, 1.41));
        check(rta::engine::Pipeline(cf).hasAnyProcessing(), "PEQ 生效时 hasAnyProcessing=true");
        rta::engine::PipelineConfig cf2;
        cf2.peq = mk(peq_flat_settings());
        check(!rta::engine::Pipeline(cf2).hasAnyProcessing(), "PEQ 全 0dB 时 hasAnyProcessing=false（保持直通）");
    }

    printf("\n%s（%d 项失败）\n", fails == 0 ? "全部通过" : "有失败项", fails);
    return fails == 0 ? 0 : 1;
}
