// room_measurement.h — 测量会话编排层（C2 包）
//
// 把 C1 的纯算法（generate_sweep / deconvolve_ir / psychoacoustic_smooth /
// compute_correction_ir / sub_alignment / room_layouts）编排成一个用户可信的
// 向导会话：逐声道 扫频→播放采集→反卷积→质检，最后批量产出可落盘的校正集。
//
// 关键设计：**传输接口注入**。会话层只认识 MeasurementTransport 这个纯虚接口，
// 真实的「WASAPI 播放 + 话筒采集」由主线实现后注入；算法/会话层不碰任何设备、
// 不 include src/audio/*（铁律 1 红线）。整个向导流程因此可以用「虚拟房间」
// （测试内卷积 + 可控噪声/增益）端到端离线验证（tests/test_room_measurement.cpp）。
//
// 质检规则（正常用户可信的向导靠这个，全部落到 res.warnings 的人话）：
//   * snrDb < 20          → 「{声道} 环境噪声偏大（SNR x dB），建议关闭空调冰箱后重测」
//   * 采集 |x| ≥ 0.999    → 「{声道} 录音接近削波，请调低话筒/功放增益重测」
//                           （在 ADC 硬削波之前预警：≥0.999 时顶部余量已不足）
//   * 布局含超低且带限互相关峰为负 → 「超低相位疑似反接，请在功放反转相位或对调接线后重测」
//   * 任一声道 valid=false → build_corrections 拒绝整批（不静默给半套校正）。
//     告警（噪声/削波/极性）是「建议重测」的人话，不判无效——结构失败才判无效，
//     否则用户在嘈杂环境里连"看看测成什么样"都做不到。
//
// 真实测量的扫频本质可闻（默认 −20 dBFS）：出声授权与警告横幅（设备名/时长/电平）
// 由主线 UI 按铁律 1 处理，本层不弹窗、不授权，只提供电平参数——
// 接线细节见 docs/测量接线说明-给主线.md。
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // write_wav_f32 用 std::fopen（同 wasapi_capture.h 的做法）
#endif
#pragma once
#include "room_correction.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace rta::dsp {

// ---------------- 传输接口 ----------------

// 传输接口：真实实现由主线接到 WASAPI 播放+采集；算法/会话层只见此接口。
// 这样整个向导流程可以用「虚拟房间」（测试内卷积+噪声）端到端验证，不碰任何设备。
struct MeasurementTransport {
    virtual ~MeasurementTransport() = default;
    // 播放 out（单声道、fs 与会话一致）并同步录制；返回录得样本（≥ samplesToCapture）。
    // 契约：实现方播放的必须是调用方传入的这同一份序列——若播放端要加淡入淡出/
    // 电平/限幅，等于改了参考信号，反卷积结果就不再是房间 IR（C1 generate_sweep
    // 注释明确过这条）。本会话已把电平乘进 out，反卷积时电平自动抵消。
    virtual std::vector<double> play_and_capture(const std::vector<double>& out,
                                                 size_t samplesToCapture) = 0;
    // 会话层注入（向导 UI 用）：stepIndex/totalSteps/channelName 进度（stepIndex 从 1 起）；
    // cancelled 返回 true 中止
    std::function<void(int, int, const char*)> onProgress;
    std::function<bool()> cancelled;
};

// ---------------- 会话数据结构 ----------------

struct ChannelMeasurement {
    std::string channelName; bool isSub = false;
    std::vector<double> roomIr;     // deconvolve_ir 结果
    double snrDb = 0.0;             // 到达前噪声窗能量 vs 到达后信号窗能量
    double peakMs = 0.0;            // 直达声位置（ir_peak_index 换算）
    bool   valid = false; std::string note;
};
struct MeasurementSessionResult {
    std::vector<ChannelMeasurement> channels;
    SubAlign subAlign;              // 布局含超低时填
    std::vector<std::string> warnings;   // 人话警告（见质检规则）
    bool   aborted = false;
};
struct CorrectionSet {              // 应用侧产物
    std::vector<std::string> channelNames;
    std::vector<std::vector<double>> correctionIrs;   // 与 channels 一一对应
    uint32_t fs = 48000; int taps = 8192;
    double subDelaySamples = 0, subGainDb = 0;         // 超低对齐参数（应用时由管线执行）
};

// ---------------- 带限互相关极性检测（超低反接质检） ----------------

// 复刻 C1 sub_alignment 的 40~120 Hz 带限互相关，只回答一个问题：按 |峰| 找到的
// 互相关极值是正还是负。反接的超低在该带内互相关呈深谷（负峰）。
// 不复用 sub_alignment 返回值的原因：它按绝对值取峰、刻意不报符号（C1 已知局限 3），
// 而本包不改既有文件，故在此独立实现同一带限互相关补上极性这一维。
// 全零带内容（异常输入）返回 false（不告警）——误报反接比漏报更折腾用户。
inline bool sub_polarity_inverted(const std::vector<double>& mainIr,
                                  const std::vector<double>& subIr, double fs) {
    if (mainIr.empty() || subIr.empty() || fs <= 0.0) return false;
    const size_t n = next_pow2(mainIr.size() + subIr.size());
    auto bandlimited = [&](const std::vector<double>& x) {
        std::vector<cplx> X(n, cplx(0.0, 0.0));
        for (size_t i = 0; i < x.size() && i < n; ++i) X[i] = cplx(x[i], 0.0);
        fft(X);
        for (size_t k = 0; k < n; ++k) {
            double f = (double)k * fs / (double)n;
            if (f > 0.5 * fs) f = fs - f;   // 负频镜像按对称频率判
            if (f < 40.0 || f > 120.0) X[k] = cplx(0.0, 0.0);
        }
        return X;
    };
    const std::vector<cplx> M = bandlimited(mainIr), S = bandlimited(subIr);
    std::vector<cplx> Xc(n);
    for (size_t k = 0; k < n; ++k) Xc[k] = std::conj(M[k]) * S[k];
    fft(Xc, true);
    double bv = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double v = Xc[i].real();
        if (std::fabs(v) > std::fabs(bv)) bv = v;
    }
    return bv < 0.0;
}

// ---------------- 测量会话 ----------------

class MeasurementSession {
public:
    // sweepLevelDbfs 默认 −20（测量扫频本质可闻）：真实会话的出声授权与横幅
    // （设备名/时长/电平/独占模式静音警告）由主线 UI 按铁律 1 处理后再调 run()。
    MeasurementSession(const SweepSpec& s, const RoomLayout& layout, double sweepLevelDbfs = -20.0)
        : spec_(s), layout_(layout), levelDbfs_(sweepLevelDbfs) {}

    // 逐声道 扫频→采集→反卷积→质检；每声道开始前检查 cancelled，
    // 中止时已完成的声道保留在结果里（aborted=true 供 UI 区分「测完」与「测了一半」）。
    MeasurementSessionResult run(MeasurementTransport& tx) const {
        MeasurementSessionResult res;
        const std::vector<double> sweep = generate_sweep(spec_);
        if (sweep.empty() || layout_.chCount <= 0) {
            res.warnings.push_back("测量配置无效（扫频参数或声道布局为空），未执行测量");
            return res;
        }
        const double gain = std::pow(10.0, levelDbfs_ / 20.0);
        std::vector<double> out(sweep.size());
        for (size_t i = 0; i < out.size(); ++i) out[i] = gain * sweep[i];
        // 多录 1 s：装下房间混响尾与扫频泄漏尾，反卷积（圆周）才不会把尾部卷绕进 IR 头部
        const size_t captureN = sweep.size() + (size_t)spec_.fs;

        for (int i = 0; i < layout_.chCount; ++i) {
            if (tx.cancelled && tx.cancelled()) { res.aborted = true; return res; }
            const char* name = layout_.chName[i] ? layout_.chName[i] : "?";
            if (tx.onProgress) tx.onProgress(i + 1, layout_.chCount, name);
            ChannelMeasurement cm;
            cm.channelName = name;
            cm.isSub = is_sub_name(name);
            measure_one(tx, out, captureN, cm, res.warnings);
            res.channels.push_back(std::move(cm));
        }
        align_sub(res);
        return res;
    }

private:
    SweepSpec spec_;
    RoomLayout layout_;
    double levelDbfs_;

    // 布局预置里超低一律叫 LFE；自定义布局沿用此约定
    static bool is_sub_name(const char* n) { return n && std::string(n) == "LFE"; }

    void measure_one(MeasurementTransport& tx, const std::vector<double>& out, size_t captureN,
                     ChannelMeasurement& cm, std::vector<std::string>& warns) const {
        const double fs = spec_.fs;
        std::vector<double> rec = tx.play_and_capture(out, captureN);
        if (rec.size() < captureN) {
            cm.valid = false;
            cm.note = "采集样本不足";
            warns.push_back(cm.channelName + " 采集样本不足（传输层提前返回），该声道测量无效");
            return;
        }
        // 削波质检放在反卷积之前：原始采集一旦触顶，后面的 IR 幅度都不可信，先记账
        bool clipped = false;
        for (double v : rec)
            if (std::fabs(v) >= 0.999) { clipped = true; break; }
        // 反卷积参考 = 实际播放的同一份序列（含电平）：H = M·conj(S)/(|S|²+ε) 中电平相消
        cm.roomIr = deconvolve_ir(rec, out, fs);
        if (cm.roomIr.empty()) {
            cm.valid = false;
            cm.note = "反卷积失败";
            warns.push_back(cm.channelName + " 反卷积失败（录音全零或扫频无效），该声道测量无效");
            return;
        }
        const size_t peak = ir_peak_index(cm.roomIr);
        cm.peakMs = (double)peak / fs * 1000.0;
        cm.snrDb = snr_db(cm.roomIr, fs, peak);
        cm.valid = true;

        if (cm.snrDb < 20.0) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.1f", cm.snrDb);
            cm.note += "信噪比不足";
            warns.push_back(cm.channelName + " 环境噪声偏大（SNR " + b +
                            " dB），建议关闭空调冰箱后重测");
        }
        if (clipped) {
            if (!cm.note.empty()) cm.note += "；";
            cm.note += "录音接近削波";
            warns.push_back(cm.channelName + " 录音接近削波，请调低话筒/功放增益重测");
        }
    }

    // SNR = 到达前噪声窗 [0, peak−5ms) 与到达后信号窗 [peak, peak+100ms) 的均方比。
    // 用均方（每样本功率）而不是总能量：两窗长度天然不同，总能量比会把窗长混进
    // SNR 里；均方才只反映「信号窗比噪声窗响多少 dB」。
    // 5 ms 守卫：跳过峰前主瓣振铃/预失真区（Farina 法的失真产物出现在峰前）。
    static double snr_db(const std::vector<double>& ir, double fs, size_t peak) {
        const size_t guard = (size_t)std::min<double>(0.005 * fs, peak);
        const size_t nn = peak - guard;
        const size_t ns = std::min((size_t)(0.1 * fs), ir.size() - peak);
        double pn = 0.0, ps = 0.0;
        for (size_t i = 0; i < nn; ++i) pn += ir[i] * ir[i];
        for (size_t i = 0; i < ns; ++i) ps += ir[peak + i] * ir[peak + i];
        if (nn == 0) {
            // 直达声在 t=0（无到达前窗）时 SNR 不可评：真实链路必有端点延迟
            // （WASAPI 往返 ≥ 数 ms），此分支只在异常配置下出现——给大值不告警，
            // 如实标注为已知局限（docs/WIP-C2.md §已知局限）。
            return 300.0;
        }
        const double mn = nn ? pn / (double)nn : 1e-300;
        const double ms = ns ? ps / (double)ns : 1e-300;
        return 10.0 * std::log10(std::max(ms, 1e-300) / std::max(mn, 1e-300));
    }

    // 布局含超低时：第一个非超低声道作主参考，与超低做带限对齐 + 极性质检。
    // 主/超低任一缺失或无效就不给对齐参数（半套数据宁可缺省，不给出错误建议）。
    void align_sub(MeasurementSessionResult& res) const {
        const ChannelMeasurement* main = nullptr;
        const ChannelMeasurement* sub = nullptr;
        for (const auto& c : res.channels) {
            if (c.isSub) { if (!sub) sub = &c; }
            else if (!main) main = &c;
        }
        if (!main || !sub || !main->valid || !sub->valid) return;
        res.subAlign = sub_alignment(main->roomIr, sub->roomIr, spec_.fs);
        if (sub_polarity_inverted(main->roomIr, sub->roomIr, spec_.fs))
            res.warnings.push_back("超低相位疑似反接，请在功放反转相位或对调接线后重测");
    }
};

// ---------------- 校正集 ----------------

// 由测量结果批量产出各声道校正 IR。
// 门禁：任一声道 valid=false → 返回空集（不静默给半套校正：只校一半的房间比
// 不校更误导，用户会以为已经校好）。
// 超低策略：超低声道自己也得到一条校正 IR（分频以下它才是主角）；subAlign 的
// 延迟/增益不烙进 IR（IR 是线性滤波器、不携带时延/增益语义），而是放在
// subDelaySamples/subGainDb 由播放管线在应用时执行（接线建议见
// docs/测量接线说明-给主线.md）。
// globalGainDb：向导的整体电平微调，线性乘到每条 IR（只改增益不改形状）。
inline CorrectionSet build_corrections(const MeasurementSessionResult& r, const TargetCurve& t,
                                       const CorrectionOpts& o, double fs,
                                       double globalGainDb = 0.0) {
    CorrectionSet cs;
    if (fs <= 0.0) return cs;
    for (const auto& ch : r.channels)
        if (!ch.valid) return CorrectionSet{};
    cs.fs = (uint32_t)fs;
    cs.taps = (int)o.taps;
    bool hasSub = false;
    for (const auto& ch : r.channels) hasSub = hasSub || ch.isSub;
    if (hasSub) {
        cs.subDelaySamples = r.subAlign.delaySamples;
        cs.subGainDb = r.subAlign.gainDb;
    }
    const double g = std::pow(10.0, globalGainDb / 20.0);
    for (const auto& ch : r.channels) {
        cs.channelNames.push_back(ch.channelName);
        std::vector<double> h = compute_correction_ir(ch.roomIr, fs, t, o);
        if (g != 1.0)
            for (double& v : h) v *= g;
        cs.correctionIrs.push_back(std::move(h));
    }
    return cs;
}

// ---------------- WAV 导出（32-bit float，文件头自写，不依赖 src/audio） ----------------

inline void wav_le_u16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
inline void wav_le_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// 写一个单声道 32-bit IEEE float WAV（format tag 3）。44 字节规范头手写：
// 本包红线是不 include src/audio，而 float WAV 的规范头恰好是定长 44 字节，
// 手写比引入写 WAV 的依赖更短、也更透明。窄字符 fopen 不认 UTF-8 路径——
// 中文目录由主线用 _wfopen+widen 包装（见 docs/测量接线说明-给主线.md），
// 或调用方直接传 ANSI/ASCII 路径。
inline std::string write_wav_f32(const std::string& path, const std::vector<double>& x, uint32_t fs) {
    if (x.empty() || x.size() > 0x0FFFFFFFull) return path + ": 样本数为空或超限";
    uint8_t h[44];
    std::memcpy(h, "RIFF", 4);
    wav_le_u32(h + 4, 36 + (uint32_t)x.size() * 4);   // RIFF 块长 = 其后全部字节
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    wav_le_u32(h + 16, 16);        // fmt 块长（PCM/float 基础格式固定 16）
    wav_le_u16(h + 20, 3);         // 3 = IEEE float
    wav_le_u16(h + 22, 1);         // 单声道
    wav_le_u32(h + 24, fs);
    wav_le_u32(h + 28, fs * 4);    // byteRate = fs × blockAlign
    wav_le_u16(h + 32, 4);         // blockAlign = 4 字节/帧
    wav_le_u16(h + 34, 32);        // 位深
    std::memcpy(h + 36, "data", 4);
    wav_le_u32(h + 40, (uint32_t)x.size() * 4);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return path + ": 无法创建文件";
    std::vector<float> buf(x.size());
    for (size_t i = 0; i < x.size(); ++i) buf[i] = (float)x[i];   // 32f WAV 本来只有 float 精度
    const bool ok = std::fwrite(h, 1, sizeof(h), f) == sizeof(h) &&
                    std::fwrite(buf.data(), 4, buf.size(), f) == buf.size();
    const bool closed = std::fclose(f) == 0;
    return (ok && closed) ? std::string() : path + ": 写入失败";
}

// 每声道写一个 32-bit float WAV（文件头自写 44 字节；文件名 dirPrefix+channelName+.wav）。
// 返回：全部成功 = 空串；失败 = 第一条错误描述（已写出的文件保留，由调用方决定去留）。
inline std::string export_corrections_wav(const CorrectionSet& cs, const std::string& dirPrefix) {
    if (cs.channelNames.size() != cs.correctionIrs.size())
        return "CorrectionSet 字段不一致：channelNames 与 correctionIrs 数量不同";
    for (size_t i = 0; i < cs.channelNames.size(); ++i) {
        const std::string err =
            write_wav_f32(dirPrefix + cs.channelNames[i] + ".wav", cs.correctionIrs[i], cs.fs);
        if (!err.empty()) return err;
    }
    return {};
}

} // namespace rta::dsp
