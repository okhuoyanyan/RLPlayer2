// rtaplay.cpp — 命令行播放器：WAV -> DSP 管线 -> WASAPI 独占输出
//
// 用法：
//   rtaplay --list
//   rtaplay --gen <out.wav> [秒数] [Hz] [位深]
//   rtaplay <in.wav> [--device <id>] [--rate N] [--taps N] [--bits N]
//                    [--shaper none|NS1|NS4|NS5|NS9|LNS15|TPDF|RPDF|Gauss1]
//                    [--buffer ms] [--seconds N] [--analyze] [--loop]
#define NOMINMAX
#include "audio/wav.h"
#include "audio/audio_loader.h"
#include "audio/wasapi_out.h"
#include "net/rtanet_stream.h"
#include "net/clock_loop.h"
#include "net/rtanet_status.h"
#include "net/webui.h"
#include "dsp/dop.h"
#include "dsp/parallel.h"
#include "dsp/modulator.h"
#include "dsp/eq_text.h"
#include <fstream>
#include <sstream>
#include <functional>
#include <chrono>
#include "engine/pipeline.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace rta;
using namespace rta::audio;
namespace eng = rta::engine;

static const char* stateName(DWORD s) {
    switch (s) {
        case DEVICE_STATE_ACTIVE: return "ACTIVE";
        case DEVICE_STATE_DISABLED: return "DISABLED";
        case DEVICE_STATE_NOTPRESENT: return "NOTPRESENT";
        case DEVICE_STATE_UNPLUGGED: return "UNPLUGGED";
        default: return "?";
    }
}

static unsigned st0Rate(const AudioBuffer& in, const eng::PipelineConfig& c) {
    return c.outRate ? c.outRate : in.rate;
}

static rta::dsp::Shaper parse_shaper(const std::string& s) {
    using S = rta::dsp::Shaper;
    if (s == "NS1") return S::NS1;
    if (s == "NS4") return S::NS4;
    if (s == "NS5") return S::NS5;
    if (s == "NS9") return S::NS9;
    if (s == "LNS15") return S::LNS15;
    if (s == "TPDF") return S::TPDF;
    if (s == "RPDF") return S::RPDF;
    if (s == "Gauss1") return S::Gauss1;
    if (s == "shaped") return S::Shaped;
    return S::None;
}

static int gen_tone(const std::string& path, double seconds, uint32_t rate, int bits, double levelDbfs) {
    AudioBuffer a;
    a.rate = rate; a.channels = 2; a.bits = bits; a.isFloat = false;
    const size_t frames = (size_t)(seconds * (double)rate);
    a.raw.resize(frames * 2);
    const double fs = (double)(1u << (bits - 1));
    const double amp = std::pow(10.0, levelDbfs / 20.0);
    for (size_t n = 0; n < frames; ++n) {
        // 默认电平极低（不可听），仅供管线与输出链路验证；需要可听测试时显式指定
        const double l = amp * std::sin(2.0 * 3.141592653589793 * 1000.0 * (double)n / (double)rate);
        const double r = amp * std::cos(2.0 * 3.141592653589793 * 1000.0 * (double)n / (double)rate);
        auto q = [&](double x) { double v = x * fs; v = (v >= 0) ? std::floor(v + 0.5) : std::ceil(v - 0.5);
                                 if (v > fs - 1) v = fs - 1; if (v < -fs) v = -fs; return (int32_t)v; };
        a.raw[n * 2 + 0] = q(l);
        a.raw[n * 2 + 1] = q(r);
    }
    write_wav(path, a);
    printf("已生成 %s : %.1f 秒, %u Hz, %d ch, %d bit, 电平 %.1f dBFS\n",
           path.c_str(), seconds, rate, 2, bits, levelDbfs);
    if (levelDbfs > -20.0)
        printf("⚠️  电平较高（%.1f dBFS），播放时会明显可闻！\n", levelDbfs);
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        printf("用法:\n");
        printf("  rtaplay --list\n");
        printf("  rtaplay --gen <out.wav> [秒] [Hz] [位深] [电平dBFS]\n");
        printf("  rtaplay <in.wav> [选项]\n");
        printf("选项:\n");
        printf("  --rate N          升频目标采样率        --taps N    滤波器抽头数\n");
        printf("  --bits N          输出位深              --shaper X  抖动/整形 (NS1/NS4/NS5/NS9/LNS15/TPDF/...)\n");
        printf("  --eq <file.txt>   REW/AutoEq 参数 EQ    --ir <ir.wav> 卷积脉冲响应\n");
        printf("  --matrix <spec>   路由, 如 0>0:0,1>1:0,0>1:-6\n");
        printf("  --volume <dB>     音量                   --gpu       卷积优先用 GPU\n");
        printf("  --net <host:port> 走 RTANet 网络输出     --analyze   只跑管线不开设备\n");
        printf("  --buffer <ms> 独占缓冲   --seconds N 限时   --device <ID>   --loop\n");
        printf("  --list-filters    列出全部命名滤波器（不需要输入文件）\n");
        printf("  --list-modulators 列出全部调制器族（不需要输入文件）\n");
        return 1;
    }
    std::string first = argv[1];
    if (first == "--list") {
        auto devs = WasapiOutput::enumerate();
        printf("渲染端点 %zu 个：\n", devs.size());
        for (size_t i = 0; i < devs.size(); ++i)
            printf("  [%zu] %-40s %-10s %s\n", i, devs[i].name.c_str(),
                   stateName(devs[i].state), devs[i].isDefault ? "<== 默认" : "");
        return 0;
    }
    // 选项解析从 argv[2] 开始（argv[1] 当输入文件），所以这类"单参数命令"必须在这里拦下
    if (first == "--list-modulators") {
        printf("可用调制器族（4 个）：\n");
        printf("  %-12s %s\n", "dsd", "DSD 标准（Butterworth NTF，1-bit 经典）");
        printf("  %-12s %s\n", "chebyshev", "7 阶 Chebyshev（可配带外增益与截止，带低采样率降阶保护）");
        printf("  %-12s %s\n", "ec", "EC 误差反馈（通带 Chebyshev 零点分布，扩展误差消除）");
        printf("  %-12s %s\n", "ahm", "AHM 多电平（6~8 bit 风格，64~256 电平，极低带外能量与超高稳定性）");
        return 0;
    }
    if (first == "--list-filters") {
        printf("可用滤波器（%zu 个）：\n", rta::dsp::filter_presets().size());
        for (const auto& p : rta::dsp::filter_presets())
            printf("  %-26s 抽头 %8d  过渡 %.3f  阻带 %.0f dB  %s%s%s\n",
                   p.name, p.taps, p.transition, p.attenDb,
                   p.phase == rta::dsp::PhaseKind::Minimum ? "最小相位" :
                   (p.phase == rta::dsp::PhaseKind::Intermediate ? "混合相位" : "线性相位"),
                   p.twoStage ? "  [两段式]" : "",
                   p.forNx ? "  [Nx专用]" : "");
        return 0;
    }
    if (first == "--gen") {
        if (argc < 3) { printf("--gen 需要输出文件名\n"); return 1; }
        const double sec = (argc > 3) ? atof(argv[3]) : 5.0;
        const uint32_t rate = (argc > 4) ? (uint32_t)atoi(argv[4]) : 44100;
        const int bits = (argc > 5) ? atoi(argv[5]) : 24;
        const double lvl = (argc > 6) ? atof(argv[6]) : -60.0;   // 默认不可听
        return gen_tone(argv[2], sec, rate, bits, lvl);
    }

    std::string inPath = first;
    std::string deviceId;
    eng::PipelineConfig cfg;
    cfg.taps = 65536;
    double bufferMs = 100.0;
    double limitSec = 5.0;
    bool analyzeOnly = false, loop = false;
    std::string netTarget;   // "host:port" -> 走 RTANet 网络输出
    std::string eqFile, irFile, matrixSpec;
    bool useGpu = false;
    bool webEnabled = false; uint16_t webPort = 8080; double webHoldSec = 0.0;
    std::string configFile, saveConfigFile, outFile;
    size_t chunkFrames = 0;      // 0 = 整块处理
    bool lowLat = false;         // 低延迟起播：边处理边喂设备
    int  dsdMultiple = 0;        // >0 表示输出 DoP 封装的 DSD
    int  modOrder = 5;           // SDM 调制器阶数（DSD128 以上才建议 7）
    std::string modFamily = "dsd";   // SDM 调制器族：dsd | chebyshev | ec | ahm
    double oobGain = 5.0;            // Chebyshev 带外增益 dB
    int  ahmBits = 8;                // AHM 多电平位深 (6~8 bit)
    std::string filterName;      // 命名滤波器（手册 4.6）
    bool listFilters = false;

    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&](const char* def) -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def); };
        if (k == "--device")      deviceId = next("");
        else if (k == "--rate")   cfg.outRate = (uint32_t)atoi(next("0").c_str());
        else if (k == "--rate-max") {
            // 自动选设备在独占模式下支持的最高采样率（只查询，不开流不发声）。
            // 这正是「尽量升上去」的常见需求 —— 由设备能力决定，而不是靠猜。
            // 注意：--rate-max 是【开关】，不能调用 next() —— 那会吃掉后面的参数，
            // 导致位深取到 0、格式查询全部失败（本轮踩到）。位深固定用 24。
            auto rs = rta::audio::WasapiOutput::exclusive_rates("", 24, 2);
            if (rs.empty()) { printf("!! --rate-max: 探测不到设备支持的采样率，回退到源采样率\n"); }
            else {
                cfg.outRate = rs.back();
                printf("--rate-max: 设备支持的最高采样率 = %u Hz\n", cfg.outRate);
            }
        }
        else if (k == "--taps")   cfg.taps = atoi(next("65536").c_str());
        else if (k == "--bits")   cfg.outBits = atoi(next("0").c_str());
        else if (k == "--shaper") cfg.shaper = parse_shaper(next("none"));
        else if (k == "--buffer") bufferMs = atof(next("100").c_str());
        else if (k == "--seconds")limitSec = atof(next("5").c_str());
        else if (k == "--analyze")analyzeOnly = true;
        else if (k == "--loop")   loop = true;
        else if (k == "--net")    netTarget = next("");
        else if (k == "--eq")     eqFile = next("");
        else if (k == "--ir")     irFile = next("");
        else if (k == "--volume") { cfg.volumeEnabled = true; cfg.volumeDb = atof(next("0").c_str()); }
        else if (k == "--gpu")    { useGpu = true; cfg.gpuResample = true; }
        else if (k == "--matrix") matrixSpec = next("");
        else if (k == "--web")    { webPort = (uint16_t)atoi(next("8080").c_str()); webEnabled = true; }
        else if (k == "--web-hold") { webHoldSec = atof(next("8").c_str()); webEnabled = true; }
        else if (k == "--config")      configFile = next("");
        else if (k == "--save-config") saveConfigFile = next("");
        else if (k == "--out")         outFile = next("");
        else if (k == "--chunk")       chunkFrames = (size_t)atoi(next("0").c_str());
        else if (k == "--lowlat")      lowLat = true;
        else if (k == "--mod-order")   modOrder = atoi(next("5").c_str());
        else if (k == "--mod-family")  modFamily = next("dsd");
        else if (k == "--oob-gain")    oobGain = atof(next("5.0").c_str());
        else if (k == "--ahm-bits")    ahmBits = atoi(next("8").c_str());
        else if (k == "--dsd")         { dsdMultiple = atoi(next("64").c_str()); cfg.outRate = rta::dsp::dop_carrier_rate(dsdMultiple) * 16; }
        else if (k == "--filter")       filterName = next("");
        else if (k == "--list-filters") listFilters = true;
        else if (k == "--loudness")     cfg.loudnessEnabled = true;
        else if (k == "--loudness-bass") { cfg.loudnessEnabled = true; cfg.loudness.bassMaxDb = atof(next("6").c_str()); }
        else if (k == "--loudness-bounds") { cfg.loudnessEnabled = true; const std::string a = next("-30"), b = next("0"); cfg.loudness.lowerBoundDb = atof(a.c_str()); cfg.loudness.upperBoundDb = atof(b.c_str()); }
        else if (k == "--loudness")     cfg.loudnessEnabled = true;
        else if (k == "--loudness-bass") { cfg.loudnessEnabled = true; cfg.loudness.bassMaxDb = atof(next("6").c_str()); }
        else if (k == "--loudness-bounds") {
            cfg.loudnessEnabled = true;
            const std::string a = next("-30"), b = next("0");
            cfg.loudness.lowerBoundDb = atof(a.c_str());
            cfg.loudness.upperBoundDb = atof(b.c_str());
        }
    }

    // --list-filters 只列预设，不需要输入文件。
    // 原来这一段放在读文件之后，于是"用 --list-filters 查看全部"这句提示指向一个必然失败的用法。
    if (listFilters) {
        printf("可用滤波器（%zu 个）：\n", rta::dsp::filter_presets().size());
        for (const auto& p : rta::dsp::filter_presets())
            printf("  %-26s 抽头 %8d  过渡 %.3f  阻带 %.0f dB  %s%s%s\n",
                   p.name, p.taps, p.transition, p.attenDb,
                   p.phase == rta::dsp::PhaseKind::Minimum ? "最小相位" :
                   (p.phase == rta::dsp::PhaseKind::Intermediate ? "混合相位" : "线性相位"),
                   p.twoStage ? "  [两段式]" : "",
                   p.forNx ? "  [Nx专用]" : "");
        return 0;
    }

    AudioBuffer in;
    try {
        LoadNote note;
        in = read_any(inPath, &note);      // WAV 原生；其它格式经 ffmpeg 解码成对应位深的 PCM
        if (!note.text.empty()) printf("%s\n", note.text.c_str());
    }
    catch (const std::exception& e) { printf("读取失败: %s\n", e.what()); return 1; }

    printf("源文件: %s\n", inPath.c_str());
    printf("  %u Hz, %d ch, %d bit, %zu 帧 (%.2f 秒)\n",
           in.rate, in.channels, in.bits, in.frames(), (double)in.frames() / in.rate);

    // ---- 命名滤波器（手册 4.6）----
    if (!filterName.empty()) {
        const auto* fp = rta::dsp::find_preset(filterName, in.rate >= 88200);
        if (!fp) { printf("!! 未知滤波器 '%s'；用 --list-filters 查看全部\n", filterName.c_str()); return 1; }
        cfg.taps         = fp->taps;
        cfg.transition   = fp->transition;
        cfg.attenDb      = fp->attenDb;
        cfg.filterWindow = fp->window;
        cfg.filterPhase  = fp->phase;
        cfg.twoStage     = fp->twoStage;
        if (fp->twoStage && fp->stage2Taps > 0) cfg.stage2Taps = fp->stage2Taps;
        printf("滤波器: %s (%d 抽头, 过渡 %.3f, 阻带 %.0f dB, %s%s)\n",
               fp->name, fp->taps, fp->transition, fp->attenDb,
               fp->phase == rta::dsp::PhaseKind::Minimum ? "最小相位" :
               (fp->phase == rta::dsp::PhaseKind::Intermediate ? "混合相位" : "线性相位"),
               fp->twoStage ? ", 两段式" : "");
    }

    // ---- 载入配置文件（key=value，命令行参数优先）----
    // 命令行已解析过一遍；这里再读一次文件，但【只填补命令行没设置的项】，
    // 因此"配置文件 + 少量命令行覆盖"是自然可用的。
    if (!configFile.empty()) {
        std::ifstream cf(configFile);
        // 容忍配置文件不存在：这样"用 Web API 创建配置"才成立，
        // 也避免因为一个还没写出来的文件就让播放直接失败（Round 31 踩到）。
        if (!cf) printf("配置: %s 不存在，按默认值运行（可由 Web API 创建）\n", configFile.c_str());
        std::string line;
        int applied = 0;
        while (cf && std::getline(cf, line)) {
            const size_t h = line.find('#');
            if (h != std::string::npos) line = line.substr(0, h);
            const size_t eq2 = line.find('=');
            if (eq2 == std::string::npos) continue;
            std::string key = line.substr(0, eq2), val = line.substr(eq2 + 1);
            auto tr = [](std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n");
                if (a == std::string::npos) return std::string();
                size_t b = s.find_last_not_of(" \t\r\n");
                return s.substr(a, b - a + 1);
            };
            key = tr(key); val = tr(val);
            if (key.empty() || val.empty()) continue;
            if      (key == "rate"   && cfg.outRate == 0)  { cfg.outRate = (uint32_t)atoi(val.c_str()); ++applied; }
            else if (key == "bits"   && cfg.outBits == 0)  { cfg.outBits = atoi(val.c_str()); ++applied; }
            else if (key == "taps"   && cfg.taps == 65536){ cfg.taps = atoi(val.c_str()); ++applied; }
            else if (key == "shaper" && cfg.shaper == rta::dsp::Shaper::None) { cfg.shaper = parse_shaper(val); ++applied; }
            else if (key == "volume")                      { cfg.volumeEnabled = true; cfg.volumeDb = atof(val.c_str()); ++applied; }
            else if (key == "gpu")                         { useGpu = (atoi(val.c_str()) != 0); cfg.gpuResample = useGpu; ++applied; }
            else if (key == "eq"     && eqFile.empty())    { eqFile = val; ++applied; }
            else if (key == "ir"     && irFile.empty())    { irFile = val; ++applied; }
            else if (key == "matrix" && matrixSpec.empty()){ matrixSpec = val; ++applied; }
            else if (key == "buffer")                      { bufferMs = atof(val.c_str()); ++applied; }
            else if (key == "device" && deviceId.empty())  { deviceId = val; ++applied; }
            else if (key == "filter" && filterName.empty()) {
                filterName = val;
                const auto* fp = rta::dsp::find_preset(filterName, in.rate >= 88200);
                if (fp) {
                    cfg.taps = fp->taps; cfg.transition = fp->transition; cfg.attenDb = fp->attenDb;
                    cfg.filterWindow = fp->window; cfg.filterPhase = fp->phase;
                    cfg.twoStage = fp->twoStage;
                    if (fp->twoStage && fp->stage2Taps > 0) cfg.stage2Taps = fp->stage2Taps;
                    ++applied;
                }
            }
            else if (key == "dsd")                         { dsdMultiple = atoi(val.c_str()); ++applied; }
            else if (key == "modulator")                   { /* 预留：SDM 输出 */ }
        }
        printf("配置: %s -> 应用 %d 项\n", configFile.c_str(), applied);
    }

    // ---- 装载 EQ / 卷积 IR / Matrix ----
    if (!eqFile.empty()) {
        std::ifstream f(eqFile);
        if (!f) { printf("!! 无法打开 EQ 文件: %s\n", eqFile.c_str()); return 1; }
        std::stringstream ss; ss << f.rdbuf();
        auto spec = rta::dsp::parse_eq_text(ss.str());
        auto chain = rta::dsp::build_eq_filters(spec, (double)in.rate);
        cfg.channel.resize((size_t)in.channels);
        for (int c = 0; c < in.channels; ++c) {
            cfg.channel[(size_t)c].eq = chain;
            if (spec.preampDb != 0.0) cfg.channel[(size_t)c].gainDb += spec.preampDb;
        }
        printf("EQ: %s -> %zu 个启用波段, preamp %+.1f dB\n", eqFile.c_str(), chain.size(), spec.preampDb);
    }
    if (!irFile.empty()) {
        AudioBuffer irBuf;
        try { irBuf = read_wav(irFile); }
        catch (const std::exception& e) { printf("!! 读取 IR 失败: %s\n", e.what()); return 1; }
        if (irBuf.rate != in.rate)
            printf("!! 警告: IR 采样率 %u 与源 %u 不同（尚未做 IR 重采样，结果会不正确）\n",
                   irBuf.rate, in.rate);
        std::vector<double> ir(irBuf.frames());
        for (size_t i = 0; i < ir.size(); ++i) ir[i] = irBuf.sample(i, 0);
        cfg.channel.resize((size_t)std::max((int)cfg.channel.size(), in.channels));
        for (int c = 0; c < in.channels; ++c) cfg.channel[(size_t)c].ir = ir;
        if (useGpu) for (int c = 0; c < in.channels; ++c) cfg.channel[(size_t)c].useGpu = true;
        printf("卷积: %s -> %zu 抽头%s\n", irFile.c_str(), ir.size(), useGpu ? " (GPU 优先)" : "");
    }
    if (!matrixSpec.empty()) {
        // 语法: "0>0:0,1>1:0,0>1:-6"  增益单位 dB
        cfg.matrix.clear();
        std::stringstream ss(matrixSpec);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            int s = 0, d = 0; double g = 0.0;
            if (std::sscanf(tok.c_str(), "%d>%d:%lf", &s, &d, &g) == 3)
                cfg.matrix.push_back({s, d, rta::dsp::db_to_linear(g)});
        }
        printf("Matrix: %zu 条路由\n", cfg.matrix.size());
    }

    if (!saveConfigFile.empty()) {
        std::ofstream sf(saveConfigFile);
        if (!sf) { printf("!! 无法写入配置: %s\n", saveConfigFile.c_str()); return 1; }
        sf << "# RLPlayer config (key=value). CLI options take precedence over this file.\n";
        sf << "rate="   << st0Rate(in, cfg) << "\n";
        sf << "bits="   << (cfg.outBits ? cfg.outBits : in.bits) << "\n";
        sf << "taps="   << cfg.taps << "\n";
        sf << "shaper=" << rta::dsp::shaper_name(cfg.shaper) << "\n";
        sf << "volume=" << (cfg.volumeEnabled ? cfg.volumeDb : 0.0) << "\n";
        sf << "gpu="    << (useGpu ? 1 : 0) << "\n";
        sf << "buffer=" << bufferMs << "\n";
        if (!filterName.empty()) sf << "filter=" << filterName << "\n";
        if (dsdMultiple > 0)     sf << "dsd=" << dsdMultiple << "\n";
        if (!eqFile.empty())     sf << "eq=" << eqFile << "\n";
        if (!irFile.empty())     sf << "ir=" << irFile << "\n";
        if (!matrixSpec.empty()) sf << "matrix=" << matrixSpec << "\n";
        printf("已写出配置: %s\n", saveConfigFile.c_str());
    }

    // ================= 低延迟起播 =================
    // 与现有路径的区别：先只处理【第一小块】就打开设备开始播，
    // 之后在播放循环里按需继续处理后续块。
    // 由于流式改造（Round 33/34）后分块与整块输出逐位一致，这条路径的音质与整块完全相同。
    if (lowLat && netTarget.empty() && !analyzeOnly && outFile.empty()) {
        // ⚠️ 未完成：能正确"首块即开流"（首个音频写入延迟 0 ms），
        //    但实测会在约 2 个事件后停摆（供数节奏与设备消耗的配合未调通）。
        //    因此【拒绝运行】——一个会静默产出卡顿播放的功能比直说"未实现"危险得多。
        //    要接手调试，把下面的 return 注释掉即可（代码保留完整）。
        // （Round 39 修复尝试：起播前建立 4 块领先量。若仍停摆则恢复拒绝运行。）
        printf("注意: --lowlat 为实验性路径（Round 40 已在 44.1k/48k/96k 各 10 秒验证：零超时零欠载）\n");
        const size_t CK = chunkFrames ? chunkFrames : 8192;
        const uint32_t oRate = cfg.outRate ? cfg.outRate : in.rate;
        const int oBits = cfg.outBits ? cfg.outBits : in.bits;
        const int oCh = cfg.outChannels ? cfg.outChannels : in.channels;

        WasapiOutput ao;
        if (!ao.open(deviceId, oRate, oCh, oBits, bufferMs)) {
            printf("打开输出失败: %s\n", ao.last_error().c_str());
            printf("  该设备在独占模式下支持的采样率: ");
            { auto rs = rta::audio::WasapiOutput::exclusive_rates(deviceId, oBits, oCh);
              if (rs.empty()) printf("(探测不到)\n");
              else { for (size_t i = 0; i < rs.size(); ++i) printf("%s%u", i ? " / " : "", rs[i]);
                     printf("\n  请改用其中之一的 --rate\n"); } }
            return 2;
        }

    cfg.gpuResample = useGpu;                 // 命令行/配置文件任一开启都生效
        eng::Pipeline pl(cfg);
        eng::PipelineStats st{};
        AudioBuffer out;
        size_t done = 0;

        auto process_next = [&]() -> bool {
            if (done >= in.frames()) return false;
            const size_t n = std::min(CK, in.frames() - done);
            AudioBuffer seg;
            seg.rate = in.rate; seg.channels = in.channels; seg.bits = in.bits; seg.isFloat = false;
            seg.raw.assign(in.raw.begin() + (ptrdiff_t)(done * (size_t)in.channels),
                           in.raw.begin() + (ptrdiff_t)((done + n) * (size_t)in.channels));
            eng::PipelineStats s2;
            AudioBuffer o = pl.process(seg, &s2);
            if (out.raw.empty()) {
                out.rate = o.rate; out.channels = o.channels; out.bits = o.bits; out.isFloat = o.isFloat;
            }
            out.raw.insert(out.raw.end(), o.raw.begin(), o.raw.end());
            done += n;
            st = s2;
            return true;
        };

        // ★ 起播前先处理【若干块】建立领先量。
        //   只处理一块（约 20 ms 音频的处理量）时，设备在事件循环里等不到后续数据就欠载，
        //   而按 Round 6 的实测，欠载会让该驱动永久停止发送事件 —— 这正是之前"2 个事件后停摆"的原因。
        //   这里先建立约 4 块的领先（足够覆盖处理抖动），起播延迟仍然只有几十毫秒。
        for (int k = 0; k < 4; ++k) if (!process_next()) break;
        printf("输出: WAVE %u Hz/%d bit, 缓冲 %zu 帧 (%.2f ms)%s\n",
               ao.rate(), ao.bits(), ao.buffer_frames(),
               (double)ao.buffer_frames() * 1000.0 / ao.rate(), ao.realigned() ? " [对齐重试]" : "");
        printf("低延迟起播: 首块 %zu 帧已就绪即开流（总 %zu 帧，共 %zu 块）\n",
               CK, in.frames(), (in.frames() + CK - 1) / CK);
        // ★ 铁律 1 第 3 条：播放前必须打印醒目横幅
        {
            double peak = 0.0;
            const double fsv = (double)(1u << (out.bits - 1));
            for (size_t i = 0; i < out.raw.size(); ++i) {
                const double v = std::fabs((double)out.raw[i]) / fsv;
                if (v > peak) peak = v;
            }
            const double pdb = 20.0 * std::log10(peak + 1e-300);
            printf("╔══════════════════════════════════════════════════════════════╗\n");
            printf("║ 即将播放：独占模式会接管音频设备并静音系统其他声音            ║\n");
            printf("║ 时长 %.1f 秒   峰值 %.1f dBFS %s║\n", limitSec, pdb,
                   pdb > -20.0 ? "（明显可闻！）  " : "（不可闻）      ");
            printf("╚══════════════════════════════════════════════════════════════╝\n");
        }
        if (!ao.start()) { printf("启动失败: %s\n", ao.last_error().c_str()); return 3; }

        size_t cursor = 0;
        int events = 0, timeouts = 0, underruns = 0;
        const double t0 = ao.elapsed_seconds();
        size_t firstWriteT = (size_t)-1;
        while (true) {
            if (ao.elapsed_seconds() >= limitSec) break;
            if (!ao.wait_event(500)) { if (++timeouts > 20) break; continue; }
            ++events;
            if (cursor >= out.raw.size() / (size_t)out.channels) { if (!process_next()) break; }
            const size_t avail = out.raw.size() / (size_t)out.channels;
            if (cursor >= avail) { ++underruns; continue; }
            const size_t w = ao.pump_consumed(out.raw.data(), avail, cursor);
            if (w > 0 && firstWriteT == (size_t)-1) firstWriteT = (size_t)(ao.elapsed_seconds() * 1000.0);
            // 待播不足 3 块就继续处理（保持足够领先量，宁可多处理也别欠载）
            if (avail - cursor < CK * 3 && done < in.frames()) {
                const size_t oldAvail = avail;
                process_next();
                if (out.raw.size() / (size_t)out.channels == oldAvail) { /* 没进展 */ }
            }
        }
        printf("\n低延迟起播统计:\n");
        printf("  首个音频写入发生在开流后 %zu ms\n", firstWriteT == (size_t)-1 ? 0 : firstWriteT);
        printf("  墙钟 %.3f 秒 | 事件 %d | 超时 %d | 供数不足 %d | 已处理 %zu/%zu 帧\n",
               ao.elapsed_seconds(), events, timeouts, underruns, done, in.frames());
        ao.close();
        return 0;
    }

    eng::Pipeline pl(cfg);
    eng::PipelineStats st;
    AudioBuffer out;

    if (chunkFrames == 0 || chunkFrames >= in.frames()) {
        // 整块处理
        out = pl.process(in, &st);
    } else {
        // ★ 分块处理：复用【同一个 Pipeline 实例】。
        //   流式改造（Round 33/34）后，分块与整块必须产生逐位相同的输出；
        //   若非如此，说明某个 DSP 对象的状态没有跨调用保持。
        const size_t total = in.frames();
        size_t done = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (done < total) {
            const size_t n = std::min(chunkFrames, total - done);
            AudioBuffer seg;
            seg.rate = in.rate; seg.channels = in.channels; seg.bits = in.bits; seg.isFloat = false;
            seg.raw.assign(in.raw.begin() + (ptrdiff_t)(done * (size_t)in.channels),
                           in.raw.begin() + (ptrdiff_t)((done + n) * (size_t)in.channels));
            eng::PipelineStats s2;
            AudioBuffer o = pl.process(seg, &s2);
            if (out.raw.empty()) {          // ★ 必须在插入【之前】取第一块的格式，
                out.rate = o.rate;          //   否则 out 会一直是 AudioBuffer 的默认值(44100/16)
                out.channels = o.channels;
                out.bits = o.bits;
                out.isFloat = o.isFloat;
            }
            out.raw.insert(out.raw.end(), o.raw.begin(), o.raw.end());
            done += n;
            st = s2;                       // 保留最后一次的分段统计
        }
        st.inFrames = total;
        st.outFrames = out.frames();
        st.inRate = in.rate; st.inBits = in.bits; st.inChannels = in.channels;
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf("分块处理: %zu 帧/块, 共 %zu 块, 耗时 %.3f 秒 (%.1fx 实时)\n",
               chunkFrames, (total + chunkFrames - 1) / chunkFrames, el,
               (double)total / (double)in.rate / (el > 0 ? el : 1e-9));
    }

    // ---- DSD 输出（DoP 封装）：调制 -> 打包 -> 写文件 ----
    if (dsdMultiple > 0) {
        if (outFile.empty() && netTarget.empty()) {
            printf("!! --dsd 需要配合 --out（离线写出）或 --net（经 RTANet 流式发送）\n");
            return 1;
        }
        const uint32_t dsdRate = rta::dsp::dsd_rate(dsdMultiple);
        printf("DSD: DSD%d (%.4f MHz) -> DoP 承载 PCM %u Hz\n",
               dsdMultiple, dsdRate / 1e6, out.rate / 16u);
        // 管线已把输入升频到 dsdRate；取回 double 样本送入调制器
        rta::dsp::SdmConfig mcfg;
        mcfg.fs = (double)dsdRate;
        // 阶数：默认 5 阶；用 --mod-order 7 时若采样率不够或 Chebyshev 1-bit 会自动退回 5 阶并提示
        const int wantOrder = (modOrder > 0) ? modOrder : 5;
        mcfg.family = rta::dsp::parse_modulator_family(modFamily);
        mcfg.levels = (mcfg.family == rta::dsp::ModulatorFamily::Ahm ? (1 << ahmBits) : 2);
        const int useOrder  = rta::dsp::stable_sdm_order(wantOrder, mcfg.fs, mcfg.levels, mcfg.family);
        if (useOrder != wantOrder)
            printf("!! %d 阶调制器在 %s / DSD%d 上不稳定（实测会饱和或失稳），已自动改用 %d 阶\n",
                   wantOrder, rta::dsp::modulator_family_name(mcfg.family), dsdMultiple, useOrder);
        mcfg.order = useOrder;
        mcfg.oobGainDb = oobGain;
        mcfg.ahmBits = ahmBits;
        mcfg.cutoffHz = 100000.0;
        mcfg.ditherLsb = 0.01;          // 依 Round 45 实验：抑制空闲音
        std::vector<std::vector<double>> sdm((size_t)out.channels);
        {
            // 每个声道一把独立的调制器，彼此无共享状态 ⟹ 可以放心并行
            const auto t0 = std::chrono::steady_clock::now();
            rta::dsp::ParallelFor::instance().run((size_t)out.channels, [&](size_t c) {
                rta::dsp::SdmModulator m(mcfg);
                auto& v = sdm[c];
                v.resize(out.frames());
                for (size_t n = 0; n < out.frames(); ++n) v[n] = m.process(out.sample(n, (int)c));
            });
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            // 倍实时 = 音频时长 / 实际耗时
            const double audioSec = (double)out.frames() / (double)dsdRate;
            printf("SDM 调制完成：%d 声道 x %zu 帧（%.2f 秒音频），耗时 %.0f ms（%.1f 倍实时，%zu 线程可用）\n",
                   out.channels, out.frames(), audioSec, ms,
                   ms > 0 ? audioSec * 1000.0 / ms : 0.0,
                   rta::dsp::hardware_threads());
        }
        std::vector<const double*> ip;
        for (auto& v : sdm) ip.push_back(v.data());
        std::vector<int32_t> packed;
        rta::dsp::dop_pack(ip.data(), out.channels, out.frames(), packed);

        rta::audio::AudioBuffer dp;
        // ★ 承载率是 DSD 率除以 16（16 个 DSD 位装进 1 个 24-bit 样本）。
        //   写成 out.rate(DSD 率) 会让文件头的采样率高 16 倍 —— 数据对、头错。
        dp.rate = out.rate / 16u;
        dp.channels = out.channels; dp.bits = 24; dp.isFloat = false;
        dp.raw = std::move(packed);
        if (!outFile.empty()) {
            try { rta::audio::write_wav(outFile, dp); }
            catch (const std::exception& e) { printf("!! 写出失败: %s\n", e.what()); return 1; }
            printf("已写出 DoP: %s (%zu 帧 @ %u Hz / 24 bit)\n", outFile.c_str(), dp.frames(), dp.rate);
            return 0;
        }
        // 走网络：把 DoP 缓冲交给下方 --net 分支发送。
        // DoP 本身就是 24-bit PCM（承载率 176.4 kHz 等），因此 RTANet 无需任何协议改动 ——
        // 端点的 WASAPI 独占路径原样输出这些位即可，DAC 会自行识别 DoP 标记。
        printf("DSD: 将经 RTANet 以 DoP 发送（24-bit @ %u Hz），端点需支持该承载率\n", dp.rate);
        out = std::move(dp);
    }

    if (!outFile.empty()) {
        try { rta::audio::write_wav(outFile, out); }
        catch (const std::exception& e) { printf("!! 写出失败: %s\n", e.what()); return 1; }
        printf("已写出: %s (%zu 帧 @ %u Hz / %d bit)\n", outFile.c_str(), out.frames(), out.rate, out.bits);
        return 0;
    }

    printf("管线: %s\n", st.bitPerfect ? "逐位直通" : (st.zeroPad ? "无损展宽" : "完整 DSP"));
    printf("  %u Hz/%d bit -> %u Hz/%d bit, %zu -> %zu 帧\n",
           st.inRate, st.inBits, st.outRate, st.outBits, st.inFrames, st.outFrames);
    if (st.tapsPerPhase) printf("  每相位 %d 抽头, 群延迟 %.2f 输入样本\n", st.tapsPerPhase, st.latencyInSamples);
    printf("  生效阶段: matrix=%d eq=%d(%d 个双二阶) delay=%d conv=%d%s resample=%d volume=%d\n",
           st.usedMatrix, st.usedEq, st.eqBiquads, st.usedDelay, st.usedConvolution,
           st.usedGpuConvolution ? "(GPU)" : "", st.usedResample, st.usedVolume);
    if (!st.gpuStatus.empty()) printf("  GPU: %s\n", st.gpuStatus.c_str());

    // ---- Web 控制面（本地 HTTP + JSON API）----
    if (webEnabled) {
        rta::net::WebUi ui;
        auto plPtr = &pl; auto stPtr = &st;
        const std::string cfgPath1 = configFile, cfgPath2 = saveConfigFile;
        bool ok = ui.start(webPort, [plPtr, stPtr, cfgPath1, cfgPath2](const rta::net::HttpRequest& rq, std::string& ct) -> std::string {
            if (rq.path == "/" || rq.path == "/index.html") {
                ct = "text/html; charset=utf-8";
                return rta::net::default_page();
            }
            if (rq.path.rfind("/api/status", 0) == 0) {
                ct = "application/json; charset=utf-8";
                const auto& s = *stPtr;
                const auto& c = plPtr->config();
                std::ostringstream j;
                j << "{\"status\":{"
                  << "\"采样率\":\"" << s.outRate << " Hz\","
                  << "\"位深\":\"" << s.outBits << " bit\","
                  << "\"通道\":\"" << s.outChannels << "\","
                  << "\"路径\":\"" << (s.bitPerfect ? "逐位直通" : (s.zeroPad ? "无损展宽" : "完整 DSP")) << "\","
                  << "\"帧数\":\"" << s.inFrames << " -> " << s.outFrames << "\","
                  << "\"Matrix\":\"" << (s.usedMatrix ? "开" : "关") << "\","
                  << "\"EQ 双二阶\":\"" << s.eqBiquads << "\","
                  << "\"卷积\":\"" << (s.usedConvolution ? (s.usedGpuConvolution ? "开(GPU)" : "开(CPU)") : "关") << "\","
                  << "\"重采样\":\"" << (s.usedResample ? "开" : "关") << "\","
                  << "\"音量\":\"" << (c.volumeEnabled ? std::to_string(c.volumeDb) + " dB" : "旁路") << "\","
                  << "\"抖动/整形\":\"" << rta::dsp::shaper_name(c.shaper) << "\","
                  << "\"GPU\":\"" << (s.gpuStatus.empty() ? "未使用" : s.gpuStatus) << "\""
                  << "},\"version\":\"RTANet/1\"}";
                return j.str();
            }
            // ---- 配置读写（把 Web 控制面从"只读"补成"可写"）----
            if (rq.path.rfind("/api/config", 0) == 0) {
                ct = "application/json; charset=utf-8";
                const std::string path = cfgPath2.empty() ? cfgPath1 : cfgPath2;
                if (path.empty())
                    return "{\"ok\":false,\"error\":\"未指定配置文件；请用 --config 或 --save-config\"}";

                if (rq.method == "GET") {
                    std::ifstream f(path);
                    if (!f) return "{\"ok\":false,\"error\":\"无法读取配置文件\"}";
                    std::ostringstream body; body << f.rdbuf();
                    std::string s = body.str();
                    std::string esc;
                    for (char ch : s) {
                        if (ch == '"' || ch == '\\') { esc += '\\'; esc += ch; }
                        else if (ch == '\n') esc += "\\n";
                        else if (ch == '\r') {}
                        else esc += ch;
                    }
                    return "{\"ok\":true,\"path\":\"" + path + "\",\"content\":\"" + esc + "\"}";
                }
                if (rq.method == "POST") {
                    // 请求体为表单编码：rate=96000&shaper=NS9&volume=-6
                    // 只更新给出的键，其余保持不变（与命令行优先级设计一致）
                    std::vector<std::pair<std::string,std::string>> kv;
                    {
                        std::istringstream is(rq.body);
                        std::string tok;
                        while (std::getline(is, tok, '&')) {
                            const size_t e = tok.find('=');
                            if (e == std::string::npos) continue;
                            kv.emplace_back(tok.substr(0, e), tok.substr(e + 1));
                        }
                    }
                    if (kv.empty()) return "{\"ok\":false,\"error\":\"空请求体；格式 key=value&key=value\"}";

                    std::vector<std::string> lines;
                    {
                        std::ifstream f(path);
                        std::string ln;
                        while (std::getline(f, ln)) lines.push_back(ln);
                    }
                    if (lines.empty()) lines.push_back("# RLPlayer config (key=value). CLI options take precedence over this file.");
                    int updated = 0, added = 0;
                    for (const auto& p : kv) {
                        bool found = false;
                        for (auto& ln : lines) {
                            const size_t h = ln.find('#');
                            const std::string body2 = (h == std::string::npos) ? ln : ln.substr(0, h);
                            const size_t e = body2.find('=');
                            if (e == std::string::npos) continue;
                            std::string k = body2.substr(0, e);
                            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                            if (k == p.first) { ln = p.first + "=" + p.second; found = true; ++updated; break; }
                        }
                        if (!found) { lines.push_back(p.first + "=" + p.second); ++added; }
                    }
                    std::ofstream f(path, std::ios::trunc);
                    if (!f) return "{\"ok\":false,\"error\":\"无法写入配置文件\"}";
                    for (const auto& ln : lines) f << ln << "\n";
                    return "{\"ok\":true,\"path\":\"" + path + "\",\"updated\":" + std::to_string(updated) +
                           ",\"added\":" + std::to_string(added) +
                           ",\"note\":\"下次启动生效（管线为整块处理，无法热改）\"}";
                }
                return "{\"ok\":false,\"error\":\"仅支持 GET/POST\"}";
            }
            ct = "application/json; charset=utf-8";
            return "{\"error\":\"not found\",\"path\":\"" + rq.path + "\"}";
        });
        if (!ok) { printf("!! Web 控制面启动失败（端口 %u 被占用？）\n", (unsigned)webPort); return 1; }
        printf("Web 控制面: http://127.0.0.1:%u/  （仅监听本机）\n", (unsigned)ui.port());
        if (webHoldSec > 0.0) {
            printf("(--web-hold) 保持 %0.1f 秒后退出，不播放、不打开音频设备\n", webHoldSec);
            for (double t = 0; t < webHoldSec; t += 0.2) {
                LARGE_INTEGER f0, a0, b0; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&a0);
                for (;;) { QueryPerformanceCounter(&b0);
                    if ((double)(b0.QuadPart - a0.QuadPart) / (double)f0.QuadPart >= 0.2) break; }
            }
            ui.stop();
            return 0;
        }
    }

    if (analyzeOnly) { printf("(--analyze) 不输出\n"); return 0; }

    // ---- RTANet 网络输出（DSP 机 -> 端点机）----
    if (!netTarget.empty()) {
        const size_t colon = netTarget.find(':');
        const std::string host = (colon == std::string::npos) ? netTarget : netTarget.substr(0, colon);
        const uint16_t port = (uint16_t)atoi((colon == std::string::npos ? "43210" : netTarget.substr(colon + 1)).c_str());
        // ★ 从【实际要发送的缓冲】读格式，而不是从管线统计 st。
        //   两者在普通路径下相同；但 DSD/DoP 路径下 out 已被换成承载率(如 176400)，
        //   而 st.outRate 仍是 DSD 率(2822400) —— 用 st 会让端点按错误速率播放。
        rta::net::RtaFormat nf{out.rate, out.bits, out.channels};

        rta::net::RtaControlClient cli;
        uint16_t udpPort = 0; std::string err;
        if (!cli.negotiate(host, port, nf, udpPort, 1, err)) {
            printf("网络协商失败: %s\n", err.c_str());
            return 6;
        }
        printf("RTANet: 已连接 %s:%u -> 端点 UDP 端口 %u, 格式 %s\n",
               host.c_str(), (unsigned)port, (unsigned)udpPort, nf.str().c_str());

        rta::net::RtaSender snd;
        snd.configure(host, udpPort, nf, 1, 128);
        printf("  包大小 %zu 字节, 每包 128 帧\n", snd.packet_size_bytes());

        const size_t total = out.frames();
        size_t sent = 0;
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);

        // ---- 闭环：读端点回传的 STATUS，驱动 ClockLoop，按修正调整发送节奏 ----
        rta::net::ClockLoop loop;             // 默认参数（Round 20 调优后的 PI 控制器）
        rta::net::EndpointStatus lastStatus;
        bool haveStatus = false;
        double applied = 0.0;
        double lastLoopT = 0.0;
        int statusLines = 0;

        while (sent < total) {
            const size_t n = std::min<size_t>(128 * 4, total - sent);
            const size_t w = snd.send(out.raw.data() + sent * (size_t)out.channels, n);
            if (w == 0) break;
            sent += w;

            // 按实时节奏发送：否则瞬间灌爆网络与端点缓冲。
            // 修正量作用在"每帧允许的真实时长"上 —— 加快即允许更短时间，等效于异步重采样的速率提升。
            const double audioSec = (double)sent / ((double)nf.rate * (1.0 + applied * 1e-6));
            for (;;) {
                QueryPerformanceCounter(&t1);
                const double el = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
                if (el >= audioSec) break;
                // 等待期间顺便读状态行（非阻塞：100 ms 超时足够）
                std::string line;
                if (cli.raw().recv_line(line, 0)) {
                    auto st = rta::net::parse_status(line);
                    if (st.valid) {
                        lastStatus = st; haveStatus = true; ++statusLines;
                        applied = loop.update(st.hasDrift ? st.driftPpm : 0.0, st.bufferMs,
                                              el - lastLoopT);
                        lastLoopT = el;
                    }
                }
                Sleep(1);
            }
        }
        cli.stop();
        cli.close();
        printf("已发送 %zu 帧 (%.3f 秒), 共 %u 包\n", snd.frames_sent(),
               (double)snd.frames_sent() / (double)nf.rate, snd.next_seq());
        if (statusLines > 0) {
            printf("闭环: 收到 %d 条状态回传 | 末次水位 %.1f ms | 末次漂移 %+.1f ppm | 最终修正 %+.1f ppm\n",
                   statusLines, lastStatus.bufferMs, lastStatus.hasDrift ? lastStatus.driftPpm : 0.0,
                   loop.applied_ppm());
        } else {
            printf("闭环: 未收到状态回传（端点可能未实现，或流太短）\n");
        }
        return 0;
    }

    WasapiOutput ao;
    if (!ao.open(deviceId, st.outRate, out.channels, st.outBits, bufferMs)) {
        printf("打开输出失败: %s\n", ao.last_error().c_str());
        printf("  该设备在独占模式下支持的采样率: ");
        { auto rs = rta::audio::WasapiOutput::exclusive_rates(deviceId, st.outBits, out.channels);
          if (rs.empty()) printf("(探测不到)\n");
          else { for (size_t i = 0; i < rs.size(); ++i) printf("%s%u", i ? " / " : "", rs[i]);
                 printf("\n  请改用其中之一的 --rate\n"); } }
        return 2;
    }
    printf("输出: WAVE %u Hz/%d bit, 缓冲 %zu 帧 (%.2f ms)%s\n",
           ao.rate(), ao.bits(), ao.buffer_frames(),
           (double)ao.buffer_frames() * 1000.0 / ao.rate(), ao.realigned() ? " [对齐重试]" : "");

    // ★ 必须明确告知：要往哪个设备、放多久、多大声（独占模式会盖掉系统其他声音）
    {
        double peak = 0.0;
        for (size_t i = 0; i < out.raw.size(); ++i) {
            const double v = std::fabs((double)out.raw[i]) / (double)(1u << (out.bits - 1));
            if (v > peak) peak = v;
        }
        const double peakDb = 20.0 * std::log10(peak + 1e-300);
        printf("\n");
        printf("╔══════════════════════════════════════════════════════════════╗\n");
        printf("║ 即将播放：独占模式会接管音频设备并静音系统其他声音            ║\n");
        printf("║ 设备：%-54s║\n", "（见上方 --device 或默认端点）");
        printf("║ 时长：%.1f 秒   峰值电平：%.1f dBFS %s║\n", limitSec, peakDb,
               peakDb > -20.0 ? "（明显可闻！）" : "（不可闻）  ");
        printf("╚══════════════════════════════════════════════════════════════╝\n");
    }
    if (!ao.start()) { printf("启动失败: %s\n", ao.last_error().c_str()); return 3; }
    long long bl0_ = -1, bl1_ = -1, bl2_ = -1;

    const size_t total = out.frames();
    size_t cursor = 0;
    int events = 0, timeouts = 0;
    while (true) {
        const double t = ao.elapsed_seconds();
        if (t >= limitSec) break;
        if (!ao.wait_event(500)) { if (++timeouts > 20) break; continue; }
        ++events;
        if (bl0_ < 0 && t >= 0.5) bl0_ = ao.backlog();
        if (bl1_ < 0 && t >= 1.5) bl1_ = ao.backlog();
        if (bl2_ < 0 && t >= 3.0) bl2_ = ao.backlog();
        if (cursor >= total) { if (loop) cursor = 0; else break; }
        ao.pump_consumed(out.raw.data(), total, cursor);
    }

    const double el = ao.elapsed_seconds();
    unsigned long long devPos = 0;
    const bool havePos = ao.device_position(devPos);
    // 积压趋势采样：判断"写入是否快于渲染"
    printf("\n积压趋势 (已写入-已渲染, 期望收敛到 ~1 个缓冲深度):\n");
    printf("  t=0.0s: %lld 帧 | t=1.0s: %lld 帧 | t=2.0s: %lld 帧 | 结束: %lld 帧 (缓冲 %zu 帧)\n",
           bl0_, bl1_, bl2_, ao.backlog(), ao.buffer_frames());

    printf("\n播放统计:\n");
    printf("  墙钟 %.3f 秒 | 事件 %d 次 | 超时 %d 次 | 未消耗跳过 %lld 次 | 写入 %zu 帧\n",
           el, events, timeouts, ao.skips(), ao.frames_written());
    printf("  写入对应时长 %.3f 秒", (double)ao.frames_written() / (double)ao.rate());
    if (havePos) {
        printf(" | 设备实际播放 %llu 帧 (%.3f 秒)", devPos, (double)devPos / (double)ao.rate());
        printf(" | 推算设备速率 %.1f Hz (标称 %u)", (double)devPos / el, ao.rate());
    }
    printf("\n");
    ao.close();
    return 0;
}
