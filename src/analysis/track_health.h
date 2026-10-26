// track_health.h — 音频体检 B1b：编排层（动态 + 假无损两分析器的统一入口 + JSON 序列化）
//
// 用途：B1a 的两个纯算法分析器（dynamics.h / fake_lossless.h）在这里合成一个「曲目体检」
//       结果：
//        * analyze_buffer   —— 吃已解码的 AudioBuffer（已缓冲场景）；
//        * analyze_file     —— 文件场景。默认【流式分块】：按块解码 → 按块喂增量分析器，
//                              进程峰值内存只与块大小有关，与曲长无关
//                              （DSD64 271 MB 实测 2588 MB → 见 tests/measure_health_mem.cpp）。
//                              拿不准能否分块读的格式/情形自动回退整段 read_any 路径。
//                              文件级入口保证不抛异常 —— 解码失败/格式不识别都落进
//                              status="error" + error 人话原因，调用方（主线 HTTP/后台 job）
//                              不需要 try/catch。
//        * analyze_file_whole —— 老实现（整曲解码）原样保留：既是对照基准，也是回退路径。
//
//       JSON 字段名与 docs/分析器API建议.md（主线接线契约）逐字一致，snake_case；
//       字符串按 JSON 规则转义（写法对齐 tools/rtaweb.cpp 的 jesc：手工拼接 + 转义，
//       不引入 JSON 生成库）。
//
// ★ 逐位一致约定：同一文件，analyze_file（流式，任意块大小）与 analyze_file_whole（整段）
//   的 dr_db / lufs / lra / peak_dbfs / cutoff_hz / confidence / fake24 必须完全相同（==）。
//   依据是 dynamics.h / fake_lossless.h 两个增量分析器的硬约定（见各自文件头），
//   以及 DsdPcmStream / WavPcmStream 与整曲解码的逐样本一致性。
//
// 已知边界（沿用 B1a，详见 docs/WIP-B1a.md §6）：
//   - 多声道 LUFS 权重只实现 L/R=1.0；素材 <3 s 时 DR/LRA 口径不成立（返回 0）；
//   - 流式覆盖 .dsf/.dff/.wav；flac/alac/mp3 等经 ffmpeg 的格式仍整曲解码
//     （ffmpeg 管道是顺序流、无法按窗回跳，谱窗计划需要"总帧数已知且可回跳"）。
//     这类文件的峰值内存未改善 —— 如实标注，不假装已覆盖。
#pragma once
#include "dynamics.h"
#include "fake_lossless.h"
#include "quality.h"                 // B2 音质分析（19 项客观指标）；增量接口，与流式体检同一次解码
#include "health_stream.h"           // 分块 PCM 源（DSD / WAV）
#include "../audio/audio_loader.h"   // read_any（整段回退路径，只读使用）
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace rta::analysis {

struct TrackHealth {
    // 动态（dynamics.h 原样透传）
    double drDb = 0.0;
    double lufs = 0.0;
    double lra = 0.0;
    double peakDbfs = 0.0;
    // 假无损（fake_lossless.h 原样透传）
    bool   losslessSuspected = false;
    double cutoffHz = 0.0;
    double confidence = 0.0;
    std::string reason;          // 中文人话，UI 直接显示
    bool   fake24bit = false;
    // 元信息
    double durationSec = 0.0;
    uint32_t fs = 0;
    int channels = 0;
    int bits = 0;
    std::string status;          // "ready" / "error"
    std::string error;           // status=error 时的人话原因
    int64_t analyzedAtMs = 0;    // Unix epoch 毫秒；缓存命中语义见 docs/分析器API建议.md
    // ---- 音质分析（B2 quality.h，2026-10-27 接入）----
    // hasQuality=false 时 quality 全 0，JSON 段里 available=false（老调用方只读老字段，零影响）。
    bool hasQuality = false;
    QualityResult  quality;
    QualityTimings qualityTimes;   // 各阶段耗时（诊断用，不进 JSON）
    // 本次是否走了流式分块路径（诊断字段，不进 JSON 契约）
    bool streamed = false;
    // 实际使用的块大小（帧；整段路径为 0）
    size_t blockFrames = 0;
};

// 流式体检选项
struct HealthStreamOptions {
    size_t blockFrames = 65536;  // 分块帧数（建议 65536~262144；取 1000 结果也一样）
    bool   stream = true;        // false = 强制整段路径（对照 / 回退 / 排错）
};

namespace detail {

inline int64_t now_ms() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// JSON 字符串转义（对齐 rtaweb.cpp 的 jesc；补全 \b \f \r 三种短转义）。
// ≥0x20 的字节原样直通 —— reason/路径里的中文按 UTF-8 字节保持不变，合法 JSON。
inline std::string json_escape(const std::string& s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

inline std::string jnum(double v, const char* fmt) {
    char b[64];
    std::snprintf(b, sizeof(b), fmt, v);
    return b;
}

// 已解码缓冲的"分块源"适配器：让 analyze_buffer 与文件流式走同一段驱动循环，
// 从而共用同一份"交错 double → 增量分析器"的喂法（逐位一致的另一个前提）。
class BufferSource {
public:
    explicit BufferSource(const rta::audio::AudioBuffer& b) : buf_(b) {}
    uint32_t rate() const { return buf_.rate; }
    int    channels() const { return buf_.channels; }
    int    bits() const { return buf_.bits; }
    bool   isFloat() const { return buf_.isFloat; }
    size_t totalFrames() const { return buf_.frames(); }
    size_t read(size_t pos, size_t n, rta::audio::AudioBuffer& out) {
        const size_t total = buf_.frames();
        if (pos >= total || n == 0) return 0;
        if (n > total - pos) n = total - pos;
        const size_t chn = (size_t)buf_.channels;
        out.rate = buf_.rate; out.channels = buf_.channels; out.bits = buf_.bits; out.isFloat = buf_.isFloat;
        out.raw.assign(buf_.raw.begin() + (long long)(pos * chn),
                       buf_.raw.begin() + (long long)((pos + n) * chn));
        return n;
    }
private:
    const rta::audio::AudioBuffer& buf_;
};

// 通用流式驱动：src 需提供 rate()/channels()/bits()/isFloat()/totalFrames()/read(pos,n,out)。
// 逐块把交错样本转成 double 喂给两个增量分析器；峰值内存 = 一个块 + 分析器常数状态。
template <class Src>
inline TrackHealth drive_health(Src& src, size_t blockFrames, bool* streamedOk) {
    TrackHealth h;
    h.fs = src.rate();
    h.channels = src.channels();
    // float 缓冲按 32 位语义处理（与 audio_loader 的位深探测口径一致）：float 无整数 LSB
    // 可言，传 32 只是让 fake24 检查有确定的整数域尺度，不会误触发（实测低 8 位近似均匀）。
    h.bits = src.isFloat() ? 32 : src.bits();
    h.analyzedAtMs = now_ms();
    if (streamedOk) *streamedOk = false;

    const size_t total = src.totalFrames();
    if (total == 0 || h.channels <= 0 || h.fs == 0) {
        h.status = "error";
        h.error = "解码后无可用样本（帧数、声道数或采样率为 0）";
        return h;
    }
    h.durationSec = (double)total / (double)h.fs;

    DynamicsAccumulator dacc;
    LosslessAccumulator lacc;
    // 音质分析（quality.h）：与另两个增量分析器吃【同一份】交错样本 —— 一次解码三份结论，
    // 不多读一遍盘。谱窗计划取决于总帧数（total 已由调用方保证），与整段接口逐位一致。
    QualityAccumulator qacc;
    dacc.begin(h.channels, (double)h.fs);
    lacc.begin(total, h.channels, (double)h.fs, h.bits);
    qacc.begin(total, h.channels, (double)h.fs);

    const size_t step = blockFrames ? blockFrames : size_t(65536);
    std::vector<double> tmp;
    rta::audio::AudioBuffer chunk;
    size_t pos = 0;
    while (pos < total) {
        const size_t n = std::min(step, total - pos);
        const size_t got = src.read(pos, n, chunk);
        if (got == 0) break;
        tmp.resize(got * (size_t)h.channels);
        for (size_t i = 0; i < got; ++i) {
            for (int c = 0; c < h.channels; ++c)
                tmp[i * (size_t)h.channels + (size_t)c] = chunk.sample(i, c);
        }
        dacc.add_interleaved(tmp.data(), got);
        lacc.add_interleaved(tmp.data(), got);
        qacc.add_interleaved(tmp.data(), got);
        pos += got;
        if (got < n) break;
    }

    const DynamicsResult d = dacc.finalize();
    const LosslessCheck lc = lacc.finalize();
    h.drDb = d.drDb;
    h.lufs = d.lufsIntegrated;
    h.lra = d.lra;
    h.peakDbfs = d.peakDbfs;
    h.losslessSuspected = lc.suspected;
    h.cutoffHz = lc.cutoffHz;
    h.confidence = lc.confidence;
    h.reason = lc.reason;
    h.fake24bit = lc.fake24bit;
    // 音质 19 项（只在帧数对得上时可信：谱窗计划靠 total，短读一律不给结论）
    h.quality = qacc.finalize(&h.qualityTimes);
    h.hasQuality = h.quality.valid;
    h.status = "ready";
    h.blockFrames = step;
    // 谱窗计划依赖总帧数：读到的帧数与声明不符（截断/短读）时计划不成立，
    // 结果不可信 —— 由调用方回退整段路径（analyze_file 会这么做）。
    const bool matched = lacc.planMatched() && dacc.framesSeen() == total;
    if (streamedOk) *streamedOk = matched;
    // streamed 只表示「这个文件是按块从磁盘读的」——analyze_buffer 内部也是分块喂，
    // 但它并没有流式解码，不能标成 streamed（诊断字段要如实）。
    h.streamed = (streamedOk != nullptr) && matched;
    if (!matched) {
        h.status = "error";
        h.error = "流式读取未取满声明帧数（" + std::to_string(dacc.framesSeen()) + "/" +
                  std::to_string(total) + "），流式结果不可信";
    }
    return h;
}

} // namespace detail

// 核心入口（解码无关）：对已在内存里的 AudioBuffer 做体检。不抛异常。
// 内部按 65536 帧分块喂增量分析器（不再铺整段 double 平面），结果与整段接口逐位相同。
inline TrackHealth analyze_buffer(const rta::audio::AudioBuffer& buf) {
    detail::BufferSource src(buf);
    return detail::drive_health(src, 65536, nullptr);
}

// 整段路径（老实现，保留作为回退与对照基准）：read_any 整曲解码 + analyze_buffer。绝不抛异常。
inline TrackHealth analyze_file_whole(const std::string& path) {
    TrackHealth h;
    try {
        rta::audio::LoadNote note;
        rta::audio::AudioBuffer buf = rta::audio::read_any(path, &note);
        return analyze_buffer(buf);
    } catch (const std::exception& e) {
        h.status = "error";
        h.error = e.what();
    } catch (...) {
        h.status = "error";
        h.error = "未知异常（非 std::exception）";
    }
    h.analyzedAtMs = detail::now_ms();
    return h;
}

// 文件级入口（流式优先）：能分块读就分块读；任何环节不成立就回退 analyze_file_whole。
// 绝不向调用方抛异常。
inline TrackHealth analyze_file(const std::string& path, const HealthStreamOptions& opt) {
    if (opt.stream) {
        detail::HealthSource src;
        std::string why;
        if (src.open(path, &why)) {
            bool ok = false;
            TrackHealth h = detail::drive_health(src, opt.blockFrames, &ok);
            if (ok && h.status == "ready") return h;
            // 短读/计划不成立/无样本：回退整段路径，用老实现的结论（错误信息也由它给）。
        }
    }
    return analyze_file_whole(path);
}

// 老签名，老调用方零改动 —— 现在默认走流式，结果与整段路径逐位相同。
inline TrackHealth analyze_file(const std::string& path) {
    return analyze_file(path, HealthStreamOptions{});
}

// JSON 序列化。字段名与 docs/分析器API建议.md 逐字一致（主线按它接线，勿改名）。
inline std::string track_health_json(const TrackHealth& h) {
    std::ostringstream o;
    o << "{\"dr_db\":"      << detail::jnum(h.drDb, "%.3f")
      << ",\"lufs\":"       << detail::jnum(h.lufs, "%.3f")
      << ",\"lra\":"        << detail::jnum(h.lra, "%.3f")
      << ",\"peak_dbfs\":"  << detail::jnum(h.peakDbfs, "%.3f")
      << ",\"lossless_suspected\":" << (h.losslessSuspected ? "true" : "false")
      << ",\"cutoff_hz\":"  << detail::jnum(h.cutoffHz, "%.1f")
      << ",\"confidence\":" << detail::jnum(h.confidence, "%.3f")
      << ",\"reason\":\""   << detail::json_escape(h.reason) << "\""
      << ",\"fake24\":"     << (h.fake24bit ? "true" : "false")
      << ",\"duration_sec\":" << detail::jnum(h.durationSec, "%.3f")
      << ",\"fs\":"         << (uint64_t)h.fs
      << ",\"channels\":"   << h.channels
      << ",\"bits\":"       << h.bits
      << ",\"status\":\""   << detail::json_escape(h.status) << "\""
      << ",\"error\":\""    << detail::json_escape(h.error) << "\""
      << ",\"analyzed_at_ms\":" << (long long)h.analyzedAtMs
      // ---- quality 段（B2 quality.h 的 19 项；2026-10-27 接入）----
      // 向后兼容：老字段一个都没动，新段只增不删。available=false 时各数值无意义（调用方按它判）。
      // 字段名与结构体一一对应（snake_case），单位写进 *_unit 注释不写进键名（与老字段风格一致）。
      << ",\"quality\":{"
      << "\"available\":"      << (h.hasQuality ? "true" : "false")
      // 1. 失真
      << ",\"thdn_db\":"       << detail::jnum(h.quality.thdnDb, "%.3f")
      << ",\"thdn_pct\":"      << detail::jnum(h.quality.thdnPct, "%.5f")
      << ",\"thdn_available\":" << (h.quality.thdnAvailable ? "true" : "false")
      << ",\"thdn_method\":\"" << detail::json_escape(h.quality.thdnMethod) << "\""
      << ",\"non_tonal_db\":"  << detail::jnum(h.quality.nonTonalDb, "%.3f")
      << ",\"imd_available\":" << (h.quality.imdAvailable ? "true" : "false")
      << ",\"imd_db\":"        << detail::jnum(h.quality.imdDb, "%.3f")
      << ",\"imd_note\":\""    << detail::json_escape(h.quality.imdNote) << "\""
      // 2. 噪声
      << ",\"noise_floor_dbfs\":" << detail::jnum(h.quality.noiseFloorDbfs, "%.3f")
      << ",\"spectral_min_dbfs\":" << detail::jnum(h.quality.spectralMinDbfs, "%.3f")
      << ",\"snr_db\":"        << detail::jnum(h.quality.snrDb, "%.3f")
      // 3. 频响
      << ",\"centroid_hz\":"   << detail::jnum(h.quality.centroidHz, "%.1f")
      << ",\"tilt_db_per_oct\":" << detail::jnum(h.quality.tiltDbPerOct, "%.3f")
      << ",\"rolloff95_hz\":"  << detail::jnum(h.quality.rolloff95Hz, "%.1f")
      << ",\"hf_ratio_10k_db\":" << detail::jnum(h.quality.hfRatio10kDb, "%.3f")
      // 4. 削波 / 峰值
      << ",\"peak_dbfs_q\":"   << detail::jnum(h.quality.peakDbfs, "%.3f")
      << ",\"rms_dbfs\":"      << detail::jnum(h.quality.rmsDbfs, "%.3f")
      << ",\"clipped_samples\":" << (unsigned long long)h.quality.clippedSamples
      << ",\"clipped_pct\":"   << detail::jnum(h.quality.clippedPct, "%.5f")
      << ",\"clipped_runs\":"  << (unsigned long long)h.quality.clippedRuns
      << ",\"longest_clip_run\":" << (unsigned long long)h.quality.longestClipRun
      << ",\"crest_db\":"      << detail::jnum(h.quality.crestDb, "%.3f")
      // 5. 立体声
      << ",\"stereo\":"        << (h.quality.stereo ? "true" : "false")
      << ",\"channel_corr\":"  << detail::jnum(h.quality.channelCorr, "%.4f")
      << ",\"mono_loss_db\":"  << detail::jnum(h.quality.monoLossDb, "%.3f")
      << ",\"side_mid_db\":"   << detail::jnum(h.quality.sideMidDb, "%.3f")
      // 6. 动态细化
      << ",\"dyn_p95_p10_db\":" << detail::jnum(h.quality.dynP95P10Db, "%.3f")
      << ",\"dyn_centroid_dbfs\":" << detail::jnum(h.quality.dynCentroidDbfs, "%.3f")
      << ",\"quiet_p10_dbfs\":" << detail::jnum(h.quality.quietP10Dbfs, "%.3f")
      << ",\"silence_pct\":"   << detail::jnum(h.quality.silencePct, "%.3f")
      // 元
      << ",\"spectral_windows\":" << (unsigned long long)h.quality.spectralWindows
      << ",\"note\":\""        << detail::json_escape(h.quality.note) << "\""
      << "}"
      << "}";
    return o.str();
}

} // namespace rta::analysis
