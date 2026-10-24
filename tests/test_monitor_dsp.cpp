// test_monitor_dsp.cpp — 「监听走 DSP」（monitor=dsp）的离线判据（不打开任何音频设备）
//
// 被测对象：src/audio/wasapi_capture.h 的 MonitorDspChain（采集块 → 现有 DSP 管线 → 输出块）
// 以及它的两个辅助纯函数：monitor_default_matrix / monitor_latency_estimate。
//
// 三项必测（父任务书要求）：
//   1) 1 kHz 正弦 + PEQ +6 dB @1k ⟹ 该频点幅度 ≈ 2 倍（±0.5 dB）；
//   2) 输入数字静音 ⟹ 输出全零；
//   3) 44.1k 采集 → 48k 输出 ⟹ 帧数比例正确（±1 帧/秒）。
//
// 判据说明（铁律 3：这个指标真的能证明要证明的事吗？）
//   * 幅度用【最小二乘正弦拟合】测量，不用时域逐点比对：后者会被重采样/滤波器的
//     延迟估计误差完全支配（播放管线纪要里的"三次假失败"就是这个坑）。
//   * 同一套测量代码先跑一遍【PEQ 全平】的对照组，必须得到 0 dB —— 否则测出来的
//     "+6 dB" 可能只是方法本身的系统偏差。
//   * 每项都额外断言"该生效的阶段真的生效了"（used_peq / used_resample），
//     避免"参数写进去了但管线走了直通"这种假通过。
//
// 电平：全程纯离线，样本只存在于内存（不写文件、不开设备），幅度 0.05（−26 dBFS）。
//      铁律 1 只约束"向听音设备播放"，这里没有任何设备参与。

#include "audio/wasapi_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
int g_fail = 0;
int g_checks = 0;

void check(bool cond, const std::string& msg) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("  [FAIL] %s\n", msg.c_str());
    } else {
        std::printf("  [ ok ] %s\n", msg.c_str());
    }
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

// 最小二乘拟合单频正弦：x[n] ≈ A·cos(w0·n/fs) + B·sin(w0·n/fs)
// 返回幅度 sqrt(A²+B²)。延迟无关（拟合的是"这段里有正弦、幅度多大"）。
struct ToneFit {
    double amp = 0.0;
    double a = 0.0, b = 0.0;
    bool ok = false;
};

ToneFit fit_tone(const std::vector<float>& x, size_t startFrame, size_t frames,
                 int channels, int ch, double fs, double f0) {
    ToneFit r;
    if (frames < 64 || channels <= 0) return r;
    const double w = 2.0 * kPi * f0 / fs;
    double cc = 0.0, ss = 0.0, cs = 0.0, xc = 0.0, xs = 0.0;
    for (size_t n = 0; n < frames; ++n) {
        const double v = (double)x[(startFrame + n) * (size_t)channels + (size_t)ch];
        const double c = std::cos(w * (double)n);
        const double s = std::sin(w * (double)n);
        cc += c * c; ss += s * s; cs += c * s;
        xc += v * c; xs += v * s;
    }
    const double det = cc * ss - cs * cs;
    if (std::fabs(det) < 1e-12) return r;
    r.a = (xc * ss - xs * cs) / det;
    r.b = (xs * cc - xc * cs) / det;
    r.amp = std::sqrt(r.a * r.a + r.b * r.b);
    r.ok = true;
    return r;
}

std::vector<float> make_sine(size_t frames, int channels, double fs, double f0, double amp) {
    std::vector<float> v(frames * (size_t)channels);
    for (size_t n = 0; n < frames; ++n) {
        const float s = (float)(amp * std::sin(2.0 * kPi * f0 * (double)n / fs));
        for (int c = 0; c < channels; ++c) v[n * (size_t)channels + (size_t)c] = s;
    }
    return v;
}

// 按实时包大小（10 ms）分块喂进去 —— 与 rtaweb 采集线程同样的调用形状
std::vector<float> run_chain(rta::audio::MonitorDspChain& chain, const std::vector<float>& in,
                             int inChannels, size_t chunkFrames) {
    std::vector<float> out;
    const size_t frames = in.empty() ? 0 : in.size() / (size_t)inChannels;
    for (size_t off = 0; off < frames; off += chunkFrames) {
        const size_t n = std::min(chunkFrames, frames - off);
        chain.process(in.data() + off * (size_t)inChannels, n, out);
    }
    return out;
}

rta::dsp::PeqSettings peq_with_gain(double freq, double gainDb, double q) {
    rta::dsp::PeqSettings s = rta::dsp::peq_flat_settings();
    // 用户在 20 段里挑第 11 段（1 kHz）抬 +6 dB；其它段保持 0
    for (auto& b : s.bands) {
        if (std::fabs(b.freq - freq) < 0.5) { b.gainDb = gainDb; b.q = q; }
    }
    return s;
}

std::shared_ptr<rta::dsp::PeqEngine> make_peq(const rta::dsp::PeqSettings& s, double fs, int channels) {
    auto e = std::make_shared<rta::dsp::PeqEngine>();
    e->set_settings(s);
    e->prepare(fs, channels);
    return e;
}

// ---------------- 判据 1：PEQ +6 dB @1k → 2 倍（±0.5 dB），并带平直对照组 ----------------
void test_peq_6db() {
    std::printf("[1] PEQ +6 dB @1 kHz：输出该频点幅度应为输入的 2 倍（±0.5 dB）\n");
    const double fs = 48000.0;
    const int ch = 2;
    const double f0 = 1000.0;
    const double amp0 = 0.05;             // −26 dBFS，纯内存
    const size_t total = (size_t)fs * 2;  // 2 秒
    const std::vector<float> in = make_sine(total, ch, fs, f0, amp0);

    // ---- 实验组：PEQ +6 dB @1k ----
    {
        rta::audio::MonitorDspConfig mc;
        mc.inRate = mc.outRate = 48000;
        mc.inChannels = mc.outChannels = ch;
        mc.volumeEnabled = true;
        mc.volumeDb = 0.0;
        mc.peq = make_peq(peq_with_gain(1000.0, 6.0, 1.41), fs, 8);
        rta::audio::MonitorDspChain chain;
        std::string err;
        check(chain.configure(mc, &err), "configure(): " + std::string(err.empty() ? "ok" : err));
        std::vector<float> out = run_chain(chain, in, ch, 480);
        check(chain.used_peq(), "管线确实执行了 PEQ 阶段（不是直通）");
        check(chain.in_rate() == 48000 && chain.out_rate() == 48000 &&
                  chain.in_channels() == ch && chain.out_channels() == ch,
              "采样率/声道数回报与配置一致");
        check(out.size() == total * (size_t)ch, fmt("同率输出帧数 = 输入帧数（%zu）", out.size() / ch));

        // 跳过前 0.5 s（IIR 建立），在 0.5~1.5 s 上拟合
        const size_t skip = (size_t)(fs * 0.5), win = (size_t)fs;
        for (int c = 0; c < ch; ++c) {
            ToneFit f = fit_tone(out, skip, win, ch, c, fs, f0);
            const double gainDb = 20.0 * std::log10(f.amp / amp0);
            check(f.ok && std::fabs(gainDb - 6.0) <= 0.5,
                  fmt("ch%d: 1 kHz 幅度 %.6f → %.6f（增益 %+.3f dB，判据 6.0±0.5）",
                      c, amp0, f.amp, gainDb));
        }
    }

    // ---- 对照组：PEQ 全平 ⟹ 必须 0 dB（证明测量方法本身没有系统偏差）----
    {
        rta::audio::MonitorDspConfig mc;
        mc.inRate = mc.outRate = 48000;
        mc.inChannels = mc.outChannels = ch;
        mc.volumeEnabled = true;
        mc.volumeDb = 0.0;
        mc.peq = make_peq(rta::dsp::peq_flat_settings(), fs, 8);
        rta::audio::MonitorDspChain chain;
        std::string err;
        check(chain.configure(mc, &err), "对照组 configure()");
        std::vector<float> out = run_chain(chain, in, ch, 480);
        const size_t skip = (size_t)(fs * 0.5), win = (size_t)fs;
        ToneFit f = fit_tone(out, skip, win, ch, 0, fs, f0);
        const double gainDb = 20.0 * std::log10(f.amp / amp0);
        check(f.ok && std::fabs(gainDb) <= 0.1,
              fmt("对照组（PEQ 全平）增益 %+.4f dB，判据 0±0.1", gainDb));
    }
}

// ---------------- 判据 2：输入数字静音 → 输出全零 ----------------
void test_silence() {
    std::printf("[2] 输入数字静音 ⟹ 输出必须全零（含 PEQ +6 dB 与音量）\n");
    const int ch = 2;
    rta::audio::MonitorDspConfig mc;
    mc.inRate = mc.outRate = 48000;
    mc.inChannels = mc.outChannels = ch;
    mc.volumeEnabled = true;
    mc.volumeDb = -6.0;
    mc.peq = make_peq(peq_with_gain(1000.0, 6.0, 1.41), 48000.0, 8);
    rta::audio::MonitorDspChain chain;
    std::string err;
    check(chain.configure(mc, &err), "configure()");
    const size_t frames = 48000;                    // 1 秒
    const std::vector<float> in(frames * (size_t)ch, 0.0f);
    std::vector<float> out = run_chain(chain, in, ch, 480);
    check(out.size() == frames * (size_t)ch, fmt("输出帧数 %zu", out.size() / ch));
    double peak = 0.0;
    size_t nonZero = 0;
    for (float v : out) {
        if (v != 0.0f) {
            ++nonZero;
            if (std::fabs((double)v) > peak) peak = std::fabs((double)v);
        }
    }
    check(nonZero == 0, fmt("全零判据：非零样本 %zu 个（峰值 %.3g）", nonZero, peak));
}

// ---------------- 判据 3：44.1k 采集 → 48k 输出，帧数比例 ----------------
void test_rate_ratio() {
    std::printf("[3] 44.1k → 48k：帧数比例必须正确（±1 帧/秒）\n");
    const uint32_t inRate = 44100, outRate = 48000;
    const int ch = 2;
    const size_t seconds = 3;
    const size_t inFrames = (size_t)inRate * seconds;      // 132300
    const std::vector<float> in = make_sine(inFrames, ch, (double)inRate, 1000.0, 0.05);

    rta::audio::MonitorDspConfig mc;
    mc.inRate = inRate;
    mc.outRate = outRate;
    mc.inChannels = mc.outChannels = ch;
    mc.volumeEnabled = true;
    mc.volumeDb = 0.0;
    rta::audio::MonitorDspChain chain;
    std::string err;
    check(chain.configure(mc, &err), "configure(): " + std::string(err.empty() ? "ok" : err));
    std::vector<float> out = run_chain(chain, in, ch, 441);   // 10 ms 包
    check(chain.used_resample(), "管线确实执行了重采样阶段（不是直通）");
    const size_t got = out.size() / (size_t)ch;
    const double expected = (double)inFrames * (double)outRate / (double)inRate;
    const double dev = (double)got - expected;
    check(std::fabs(dev) <= (double)seconds,    // ±1 帧/秒
          fmt("%zu 帧@44.1k → %zu 帧@48k（理论 %.0f，偏差 %+.1f 帧，容差 ±%zu）",
              inFrames, got, expected, dev, seconds));
    check(chain.taps_per_phase() >= 8 && chain.taps_total() > 0,
          fmt("重采样滤波器：每相位 %d 抽头 / 内插域共 %d 抽头",
              chain.taps_per_phase(), chain.taps_total()));
    std::printf("       重采样群延迟估计 %.3f ms；outRate 回报 %u\n",
                chain.resample_latency_ms(), (unsigned)chain.out_rate());
    // 输出必须是有限值（重采样器数值稳定性）
    size_t bad = 0;
    for (float v : out) if (!std::isfinite((double)v)) ++bad;
    check(bad == 0, fmt("输出无非有限值（bad=%zu）", bad));
}

// ---------------- 路由：单声道麦克风 → 双声道输出（旧 raw 直通的行为）----------------
void test_mono_matrix() {
    std::printf("[4] 单声道采集 → 双声道输出：左右都要有同一个信号（与旧 raw 直通一致）\n");
    const auto routes = rta::audio::monitor_default_matrix(1, 2);
    check(routes.size() == 2 && routes[0].src == 0 && routes[0].dst == 0 && routes[1].dst == 1,
          fmt("默认路由 = 1→2 复制（%zu 条）", routes.size()));
    check(rta::audio::monitor_default_matrix(2, 2).empty(), "2→2 时返回空 = 管线单位路由");
    check(rta::audio::monitor_default_matrix(2, 6).empty(), "2→6 时返回空 = 前两声道单位路由");

    const double fs = 48000.0;
    const size_t frames = (size_t)fs;
    std::vector<float> in = make_sine(frames, 1, fs, 1000.0, 0.05);
    rta::audio::MonitorDspConfig mc;
    mc.inRate = mc.outRate = 48000;
    mc.inChannels = 1;
    mc.outChannels = 2;
    mc.volumeEnabled = true;
    mc.volumeDb = 0.0;
    rta::audio::MonitorDspChain chain;
    std::string err;
    check(chain.configure(mc, &err), "configure()");
    std::vector<float> out = run_chain(chain, in, 1, 480);
    check(out.size() == frames * 2, fmt("输出帧数 %zu（应为 %zu）", out.size() / 2, frames));
    const ToneFit l = fit_tone(out, 4800, 24000, 2, 0, fs, 1000.0);
    const ToneFit r = fit_tone(out, 4800, 24000, 2, 1, fs, 1000.0);
    check(std::fabs(l.amp - 0.05) <= 0.0005 && std::fabs(r.amp - 0.05) <= 0.0005,
          fmt("左右幅度 L=%.5f R=%.5f（应各 ≈0.05）", l.amp, r.amp));
    check(std::fabs(l.amp - r.amp) <= 1e-6, "左右严格相等（同一源的复制）");
}

// ---------------- 延迟估计：口径固定（rtaweb 只负责取数）----------------
void test_latency() {
    std::printf("[5] 端到端延迟估计口径\n");
    const rta::audio::MonitorLatency l =
        rta::audio::monitor_latency_estimate(100.0, 10.0, 1.5, 0.4, 30.0, 200.0);
    check(std::fabs(l.totalMs - 311.9) < 1e-9,
          fmt("total = 采集100 + FIFO10 + 重采样1.5 + DSP0.4 + 输出缓冲200 = %.2f ms", l.totalMs));
    check(std::fabs(l.outputFillMs - 30.0) < 1e-9,
          "实测待播 30ms 单列回报，不参与求和（否则输出缓冲会被算两遍）");
    check(std::fabs(l.liveMs - 141.9) < 1e-9,
          fmt("实测占用口径 live = 100+10+1.5+0.4+30 = %.2f ms（当前真的排队量）", l.liveMs));
    const rta::audio::MonitorLatency c =
        rta::audio::monitor_latency_estimate(0.0, -5.0, 0.0, 0.0, 300.0, 200.0);
    check(c.fifoMs == 0.0 && c.outputFillMs == 200.0 && c.totalMs == 200.0 && c.liveMs == 200.0,
          fmt("负值归零 + 待播不超过缓冲深度（total=%.1f, live=%.1f）", c.totalMs, c.liveMs));
}

// ---------------- 配置错误要明确报（不许静默）----------------
void test_config_errors() {
    std::printf("[6] 非法配置必须报错（不静默）\n");
    rta::audio::MonitorDspChain chain;
    std::string err;
    rta::audio::MonitorDspConfig mc;
    mc.inRate = 0;
    const bool bad1 = !chain.configure(mc, &err);   // 先调用，再拼消息（实参求值顺序不确定）
    check(bad1 && err.find("采样率") != std::string::npos, "inRate=0 → " + err);
    err.clear();
    mc = rta::audio::MonitorDspConfig{};
    mc.outChannels = 0;
    const bool bad2 = !chain.configure(mc, &err);
    check(bad2 && err.find("声道数") != std::string::npos, "outChannels=0 → " + err);
}

// ---------------- 起流防爆音：音量渐变不得在开头漏出 0 dB 的声音 ----------------
// 判据：音量设成 −144 dB（静音）后，喂进去的第一块【满幅】正弦，输出必须逐样本为 0。
// 如果没有 configure() 里的"预跑静音"，VolumeControl 会从 1.0（0 dB）开始渐变，
// 头 50ms 的满幅信号会原样漏出去 —— 真机上就是开麦瞬间的一记爆音。
void test_no_startup_blast() {
    std::printf("[7] 起流防爆音：音量 −144 dB 时，第一块满幅输入不得漏音\n");
    const int ch = 2;
    rta::audio::MonitorDspConfig mc;
    mc.inRate = mc.outRate = 48000;
    mc.inChannels = mc.outChannels = ch;
    mc.volumeEnabled = true;
    mc.volumeDb = -144.0;                 // VolumeControl 语义：≤ −144 dB = 精确 0
    mc.peq = make_peq(peq_with_gain(1000.0, 6.0, 1.41), 48000.0, 8);
    rta::audio::MonitorDspChain chain;
    std::string err;
    check(chain.configure(mc, &err), "configure()");
    std::vector<float> in = make_sine(4800, ch, 48000.0, 1000.0, 1.0);   // 满幅 100ms
    std::vector<float> out;
    chain.process(in.data(), 4800, out);
    double peak = 0.0;
    for (float v : out) peak = std::max(peak, (double)std::fabs(v));
    check(out.size() == 4800 * (size_t)ch && peak == 0.0,
          fmt("第一块输出帧数 %zu、峰值 %.3g（必须逐样本 0）", out.size() / ch, peak));

    // 判据自证（铁律 3）：关掉预跑就必须漏音 —— 否则上面那条"峰值 0"可能只是
    // 因为测试根本没喂进满幅信号，而不是防爆音真的生效。
    mc.primeMs = 0.0;
    rta::audio::MonitorDspChain raw;
    check(raw.configure(mc, &err), "对照组 configure()（关掉预跑）");
    std::vector<float> outRaw;
    raw.process(in.data(), 4800, outRaw);
    double peakRaw = 0.0;
    for (float v : outRaw) peakRaw = std::max(peakRaw, (double)std::fabs(v));
    check(peakRaw > 0.5,
          fmt("对照组（primeMs=0）第一块峰值 %.3f（应明显 > 0.5，证明该漏音真实存在）", peakRaw));
}

} // namespace

int main() {
    std::printf("=== 监听走 DSP 离线判据（不打开音频设备）===\n");
    test_peq_6db();
    test_silence();
    test_rate_ratio();
    test_mono_matrix();
    test_latency();
    test_config_errors();
    test_no_startup_blast();
    std::printf("--------------------------------------------\n");
    std::printf("%s：%d/%d 项通过\n", g_fail == 0 ? "PASS" : "FAIL", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
