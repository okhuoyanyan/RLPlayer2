// test_channel_gain.cpp — 每声道增益与多声道路由
//
// 判据：管线里给某个输出声道一个 -6.0206 dB（正好 0.5 倍）的增益，
// 该声道的样本必须变成原来的 0.5 倍，其它声道必须【逐位不变】。
// 多声道：2ch 源升成 6ch 输出时，前两路就是原信号，其余为静音。
#include "engine/pipeline.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace rta::engine;
using rta::audio::AudioBuffer;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// 常数样本：0.5 满幅（bits 位定标）
static AudioBuffer make_const(uint32_t rate, int ch, int bits, size_t frames) {
    AudioBuffer a; a.rate = rate; a.channels = ch; a.bits = bits; a.isFloat = false;
    const int32_t v = (int32_t)((1LL << (bits - 1)) / 2);
    a.raw.assign(frames * (size_t)ch, v);
    return a;
}

int main() {
    const double kHalfDb = -6.020599913279624;   // 20*log10(0.5)

    printf("=== 1. 单声道增益只影响该声道，其余逐位不变 ===\n");
    {
        PipelineConfig cfg;
        cfg.shaper = rta::dsp::Shaper::None;
        cfg.channel.resize(2);
        cfg.channel[0].gainDb = kHalfDb;
        rta::engine::Pipeline p(cfg);
        auto in = make_const(48000, 2, 24, 1024);
        PipelineStats st;
        auto out = p.process(in, &st);
        const int32_t want0 = in.raw[0] / 2;
        bool ok0 = true, ok1 = true;
        for (size_t n = 0; n < 1024; ++n) {
            if (std::llabs((long long)out.raw[n * 2 + 0] - want0) > 2) ok0 = false;   // 舍入允许 2 LSB
            if (out.raw[n * 2 + 1] != in.raw[n * 2 + 1]) ok1 = false;
        }
        check(ok0, "声道 0 变成 0.5 倍", "");
        check(ok1, "声道 1 逐位不变", "");
        check(st.usedChannelGain, "统计里记录了用了每声道增益", "");
    }

    printf("\n=== 2. 6 声道：只给第 4 路（索引 3）-12 dB ===\n");
    {
        PipelineConfig cfg;
        cfg.shaper = rta::dsp::Shaper::None;
        cfg.channel.resize(6);
        cfg.channel[3].gainDb = -12.0;
        rta::engine::Pipeline p(cfg);
        auto in = make_const(48000, 6, 24, 256);
        auto out = p.process(in);
        const double g = std::pow(10.0, -12.0 / 20.0);
        const double want = (double)in.raw[0] * g;
        bool okGain = std::fabs((double)out.raw[3] - want) <= 4.0;
        bool others = true;
        for (int c = 0; c < 6; ++c) {
            if (c == 3) continue;
            if (out.raw[(size_t)c] != in.raw[(size_t)c]) others = false;
        }
        check(okGain, "第 4 路按 -12 dB 缩放", "");
        check(others, "其余 5 路逐位不变", "");
    }

    printf("\n=== 3. 2ch 源 -> 6ch 输出（上混为补静音） ===\n");
    {
        PipelineConfig cfg;
        cfg.shaper = rta::dsp::Shaper::None;
        cfg.outChannels = 6;
        rta::engine::Pipeline p(cfg);
        auto in = make_const(48000, 2, 24, 256);
        auto out = p.process(in);
        check(out.channels == 6, "输出声道数为 6", "实际 " + std::to_string(out.channels));
        bool front = true, rest = true;
        for (size_t n = 0; n < 256; ++n) {
            for (int c = 0; c < 2; ++c)
                if (out.raw[n * 6 + (size_t)c] != in.raw[n * 2 + (size_t)c]) front = false;
            for (int c = 2; c < 6; ++c)
                if (out.raw[n * 6 + (size_t)c] != 0) rest = false;
        }
        check(front, "前两路与源逐位相同", "");
        check(rest, "其余四路为静音", "");
    }

    printf("\n%s\n", fails == 0 ? "全部通过" : (std::to_string(fails) + " 项失败").c_str());
    return fails == 0 ? 0 : 1;
}
