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
#include "container_duration.h"      // 容器声明的时长（BUG-3：判「半截文件」，只读文件头）
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
    // 真峰（True Peak，4× 过采样，dBTP）。样本峰 ≤0 dBFS，真峰可以 >0 —— UI 的过载
    // 提示只能看这个（2026-10-27 标定：09_reverie 样本峰 0.00、真峰 +2.5）。
    double truePeakDbtp = 0.0;
    // 假无损（fake_lossless.h 原样透传）
    bool   losslessSuspected = false;
    double cutoffHz = 0.0;
    double confidence = 0.0;
    std::string reason;          // 中文人话，UI 直接显示
    bool   fake24bit = false;
    // 判定档：0 未发现 / 1 疑似有损 / 2 有效带宽受限（不判假）/ 3 证据不足
    // （与 fake_lossless.h 的 kFakeVerdict* 一一对应；UI 不再自己拿比值/置信度猜）
    int    fakeVerdict = 0;
    double edgeDb = 0.0;         // 陡沿（各窗中位数，dB）
    int    windows = 0;          // 参与分析的谱窗数
    double windowFill = 1.0;     // 谱窗填充率（<0.5 = 素材过短）
    // 元信息
    double durationSec = 0.0;
    uint32_t fs = 0;
    int channels = 0;
    int bits = 0;
    std::string status;          // "ready" / "error" / "incomplete"（BUG-3）
    std::string error;           // status=error 时的人话原因
    // ---- 完整性（BUG-3，2026-10-27）----
    // 半截文件（正在拷贝/下载、下载中断、边写边分析）能正常解码，durationSec 只反映
    // 「已有字节」的长度 ⟹ 指标看着全对、其实是半份结论。这里把**容器声明的时长**
    // （FLAC STREAMINFO / WAV data / MP3 Xing / DSF sampleCount）与实际解码时长比对，
    // 差得太多就把 status 从 "ready" 降为 "incomplete" 并写 note（指标照给，不拒绝出数）。
    // declaredSec==0 表示容器时长未知（不判）：宁可漏报，不许把正常文件标黄。
    double declaredSec = 0.0;
    bool   incomplete = false;
    std::string note;            // 不完整提示（中文人话；incomplete=false 时为空）
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

// 判定档 → 字符串（与 fake_lossless.h 的 kFakeVerdict* 对应；UI 直接按它分色/分文案）
inline const char* fake_verdict_tag(int v) {
    switch (v) {
        case 1:  return "lossy";         // 疑似有损转码（判假）
        case 2:  return "band_limited";  // 有效带宽受限（不判假）
        case 3:  return "unclear";       // 证据不足
        default: return "none";          // 未发现
    }
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
    h.truePeakDbtp = d.truePeakDbtp;
    h.losslessSuspected = lc.suspected;
    h.cutoffHz = lc.cutoffHz;
    h.confidence = lc.confidence;
    h.reason = lc.reason;
    h.fake24bit = lc.fake24bit;
    h.fakeVerdict = lc.fakeVerdict;
    h.edgeDb = lc.edgeDb;
    h.windows = lc.windows;
    h.windowFill = lc.windowFill;
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

// ---- BUG-3：完整性判定（容器声明时长 vs 实际解码时长）----
// 判据（本轮与用户约定）：|声明 − 实际| > max(1 s, 2%×声明) 才算「可能不完整」。
//   * 1 s 下限：挡住容器字段的正常粒度差（MP3 帧边界 26 ms、DSF 块填充、ape 尾部标签…）；
//   * 2% 相对量：长曲（DSD 383 s、WAV 1398 s）里 1 s 只有千分之几，光看绝对量会漏。
// ρ 实测（build-hfix，656 首库里抽 20 首 + 全 media 夹具）：所有完整文件的 |差| ≤ 0.02 s，
// 相对 0.00%，离 1 s 门槛有 50 倍余量 ⟹ 不会误报；而截断到 50% 的 FLAC 差 17.104 s（53.4%）。
inline void apply_completeness(TrackHealth& h, const std::string& path) {
    if (h.status != "ready" || !(h.durationSec > 0.0)) return;
    DeclaredDuration d;
    if (!probe_declared_duration(path, &d)) return;      // 容器时长未知 ⟹ 不判（宁可漏报）
    if (!(d.seconds > 0.0) || !std::isfinite(d.seconds)) return;
    h.declaredSec = d.seconds;
    const double diff = h.durationSec - d.seconds;
    // 用 (std::max) 的括号形式：本头文件可能被已 include 过 windows.h 的 TU 间接包含，
    // 那时 min/max 是宏，std::max(…) 会被宏展开成语法垃圾（clang 报 "too many errors"）。
    const double tol  = (std::max)(1.0, d.seconds * 0.02);
    if (std::fabs(diff) <= tol) return;                  // 在噪声内：当完整
    h.incomplete = true;
    h.status = "incomplete";
    char note[320] = {0};
    std::snprintf(note, sizeof(note),
                  "文件可能不完整：容器声明 %.3f s，实际解码 %.3f s（%.1f%%）—— "
                  "若这首正在拷贝/下载，请等复制完成后再分析一次",
                  d.seconds, h.durationSec, 100.0 * h.durationSec / d.seconds);
    h.note = note;
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
        TrackHealth h = analyze_buffer(buf);
        detail::apply_completeness(h, path);     // BUG-3：半截文件 → incomplete（指标照给）
        return h;
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
            if (ok && h.status == "ready") {
                detail::apply_completeness(h, path);   // BUG-3：流式路径同样要判（WAV/DSF）
                return h;
            }
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
      << ",\"true_peak_dbtp\":" << detail::jnum(h.truePeakDbtp, "%.3f")
      << ",\"lossless_suspected\":" << (h.losslessSuspected ? "true" : "false")
      << ",\"cutoff_hz\":"  << detail::jnum(h.cutoffHz, "%.1f")
      << ",\"confidence\":" << detail::jnum(h.confidence, "%.3f")
      // 判定档 + 判据量（2026-10-27 修复轮新增；老字段一个没动，纯附加）
      << ",\"fake_verdict\":\"" << detail::fake_verdict_tag(h.fakeVerdict) << "\""
      << ",\"edge_db\":"   << detail::jnum(h.edgeDb, "%.1f")
      << ",\"spectral_windows\":" << h.windows
      << ",\"window_fill\":" << detail::jnum(h.windowFill, "%.4f")
      << ",\"reason\":\""   << detail::json_escape(h.reason) << "\""
      << ",\"fake24\":"     << (h.fake24bit ? "true" : "false")
      << ",\"duration_sec\":" << detail::jnum(h.durationSec, "%.3f")
      << ",\"fs\":"         << (uint64_t)h.fs
      << ",\"channels\":"   << h.channels
      << ",\"bits\":"       << h.bits
      << ",\"status\":\""   << detail::json_escape(h.status) << "\""
      << ",\"error\":\""    << detail::json_escape(h.error) << "\""
      // ---- 完整性（BUG-3，2026-10-27；纯附加，老字段一个没动）----
      // declared_sec：容器声明的时长（0=未知，未判）；incomplete：是否判定为"半截文件"；
      // note：中文人话提示（incomplete=false 时为空串）。指标照给，只是让状态可见。
      << ",\"declared_sec\":" << detail::jnum(h.declaredSec, "%.3f")
      << ",\"incomplete\":"  << (h.incomplete ? "true" : "false")
      << ",\"note\":\""     << detail::json_escape(h.note) << "\""
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
