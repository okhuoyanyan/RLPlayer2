// rg_probe.cpp — ReplayGain / Album Gain 独立验证程序（只读，不写任何用户文件、不开音频设备）
//
// 用法：
//   rg_probe <文件或目录...> [--mode track|album] [--json]
//            [--compute off|missing|all] [--reference -18] [--no-prevent-clipping]
//            [--list]（只列标签，不解码）
//
// 输出每首：
//   * 标签里的 gain/peak **原始字符串** + 实际读到的**键名**（按容器）；
//   * 解析后的数值（dB / 线性 / dBFS）；
//   * 计算值（ffmpeg 解码 → 本仓库 analysis::DynamicsAccumulator 的 BS.1770-4 LUFS + 峰值）；
//   * 最终 applied_gain、是否被削波保护改过。
//
// 纪律：只读标签与音频；不解码到磁盘（ffmpeg 输出走内存管道）；不打开任何音频设备（铁律 1）。
#include "dsp/replaygain.h"
#include "analysis/dynamics.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace rta::dsp::replaygain;

// ---------------------------------------------------------------------------
// 子进程（ffmpeg / ffprobe）：
//   * 命令行用宽字符构造 —— 本库文件名大量中日文，_popen 走 ANSI 会踩「Illegal byte sequence」
//     （反馈 20261022-01 的退出码 -42 就是它）；
//   * stdout 用管道读进内存，绝不落临时文件（不改用户目录、不占磁盘）。
// ---------------------------------------------------------------------------
#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static bool run_capture(const std::vector<std::string>& args, std::vector<unsigned char>* out, int* exitCode) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return false;
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd;
    for (const auto& a : args) {
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"';
        cmd += widen(a);
        cmd += L'"';
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    const BOOL ok = ::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(wr);
    if (!ok) { ::CloseHandle(rd); return false; }
    unsigned char buf[1 << 16];
    DWORD got = 0;
    while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
        out->insert(out->end(), buf, buf + got);
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(rd);
    if (exitCode) *exitCode = (int)code;
    return true;
}
#endif

// ffprobe 取采样率/声道数（只读，不解码）
static bool probe_stream(const std::string& path, int* rate, int* channels) {
#ifdef _WIN32
    std::vector<unsigned char> out;
    int code = 0;
    if (!run_capture({"ffprobe", "-v", "error", "-select_streams", "a:0",
                      "-show_entries", "stream=sample_rate,channels",
                      "-of", "csv=p=0", path}, &out, &code) || code != 0) return false;
    std::string s(out.begin(), out.end());
    int r = 0, c = 0;
    if (std::sscanf(s.c_str(), "%d,%d", &r, &c) == 2 && r > 0 && c > 0) { *rate = r; *channels = c; return true; }
    if (std::sscanf(s.c_str(), "%d", &r) == 1 && r > 0) { *rate = r; *channels = 1; return true; }
    return false;
#else
    (void)path; (void)rate; (void)channels; return false;
#endif
}

// 解码 → 增量 LUFS/峰值（不落盘、整曲不驻留内存）
static bool measure_file(const std::string& path, double* lufs, double* peakLinear, double* durationSec) {
#ifdef _WIN32
    int rate = 0, ch = 0;
    if (!probe_stream(path, &rate, &ch)) return false;
    std::vector<unsigned char> raw;
    int code = 0;
    if (!run_capture({"ffmpeg", "-v", "error", "-i", path, "-map", "0:a:0",
                      "-f", "f64le", "-acodec", "pcm_f64le", "-"}, &raw, &code) || code != 0) return false;
    const size_t frameBytes = sizeof(double) * (size_t)ch;
    if (raw.size() < frameBytes) return false;
    rta::analysis::DynamicsAccumulator acc;
    acc.begin(ch, (double)rate);
    const size_t totalFrames = raw.size() / frameBytes;
    const size_t chunkFrames = 32768;
    std::vector<double> buf((size_t)ch * chunkFrames);
    for (size_t pos = 0; pos < totalFrames; pos += chunkFrames) {
        const size_t n = std::min(chunkFrames, totalFrames - pos);
        std::memcpy(buf.data(), raw.data() + pos * frameBytes, n * frameBytes);
        acc.add_interleaved(buf.data(), n);
    }
    const auto r = acc.finalize();
    *lufs = r.lufsIntegrated;
    *peakLinear = (r.peakDbfs > -299.0) ? std::pow(10.0, r.peakDbfs / 20.0) : 0.0;
    if (durationSec) *durationSec = (double)totalFrames / (double)rate;
    return r.valid;
#else
    (void)path; (void)lufs; (void)peakLinear; (void)durationSec; return false;
#endif
}

// ---------------------------------------------------------------------------
// 把多个文件**当成一条信号**喂进同一个累加器 ⟹ 得到"拼接整张专辑、统一门限"的 LUFS。
// 这是 Album Gain 的规范口径（RG2 的 album gain 就是整张专辑一个响度），
// 与"逐曲 LUFS 的时长加权平均"不是同一个数（见 replaygain.h 的说明与实测差）。
static bool measure_album_exact(const std::vector<std::string>& paths, double* lufs, int* usedCh, double* usedRate) {
#ifdef _WIN32
    rta::analysis::DynamicsAccumulator acc;
    bool begun = false;
    int ch0 = 0, rate0 = 0;
    for (const auto& path : paths) {
        int rate = 0, ch = 0;
        if (!probe_stream(path, &rate, &ch)) continue;
        if (!begun) { acc.begin(ch, (double)rate); begun = true; ch0 = ch; rate0 = rate; }
        else if (ch != ch0 || rate != rate0) return false;     // 同专辑格式不一致：不做拼接口径
        std::vector<unsigned char> raw;
        int code = 0;
        if (!run_capture({"ffmpeg", "-v", "error", "-i", path, "-map", "0:a:0",
                          "-f", "f64le", "-acodec", "pcm_f64le", "-"}, &raw, &code) || code != 0) continue;
        const size_t fb = sizeof(double) * (size_t)ch;
        const size_t frames = raw.size() / fb;
        const size_t chunk = 32768;
        std::vector<double> buf((size_t)ch * chunk);
        for (size_t pos = 0; pos < frames; pos += chunk) {
            const size_t n = std::min(chunk, frames - pos);
            std::memcpy(buf.data(), raw.data() + pos * fb, n * fb);
            acc.add_interleaved(buf.data(), n);
        }
    }
    if (!begun) return false;
    const auto r = acc.finalize();
    *lufs = r.lufsIntegrated;
    if (usedCh) *usedCh = ch0;
    if (usedRate) *usedRate = rate0;
    return r.valid;
#else
    (void)paths; (void)lufs; (void)usedCh; (void)usedRate; return false;
#endif
}

// 收集文件
// ---------------------------------------------------------------------------
static bool is_audio_ext(const std::string& e) {
    static const char* k[] = {".flac", ".wav", ".wave", ".mp3", ".ogg", ".oga", ".opus",
                              ".m4a", ".mp4", ".aac", ".dsf", ".dff", ".wv", ".ape"};
    for (const char* kk : k) if (e == kk) return true;
    return false;
}

static std::string u8str(const fs::path& x) {
    const auto s = x.u8string();
    return std::string((const char*)s.data(), s.size());
}

// UTF-8 字符串 → path（C++20 起 u8path 已弃用，直接构造 u8string）
static fs::path u8path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

static void collect(const std::string& p, std::vector<std::string>* out) {
    std::error_code ec;
    if (fs::is_directory(u8path(p), ec)) {
        std::vector<std::string> sub;
        for (auto it = fs::recursive_directory_iterator(u8path(p), ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string ext = u8str(it->path().extension());
            for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            if (!is_audio_ext(ext)) continue;
            sub.push_back(u8str(it->path()));
        }
        std::sort(sub.begin(), sub.end());
        out->insert(out->end(), sub.begin(), sub.end());
    } else if (fs::is_regular_file(u8path(p), ec)) {
        out->push_back(p);
    }
}

static std::string json_esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

struct Row {
    std::string path;
    Track tr;
    Decision dec;
    bool computed = false;
    double computedTrackGain = 0.0;
    bool albumComputed = false;
    double albumGainComputed = 0.0;
    double albumLufs = 0.0;
    std::string albumKey;
    size_t albumSize = 0;
    double tagTrackGain = 0.0, tagTrackPeak = 0.0, tagAlbumGain = 0.0, tagAlbumPeak = 0.0;
    bool haveTagTrackGain = false, haveTagTrackPeak = false, haveTagAlbumGain = false, haveTagAlbumPeak = false;
};

static int run_main(const std::vector<std::string>& argv) {
    const int argc = (int)argv.size();
    std::vector<std::string> inputs;
    Mode mode = Mode::Off;
    bool json = false, listOnly = false, preventClip = true, albumExact = false;
    std::string compute = "missing";     // off | missing | all
    double ref = kReferenceLufs;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--list") { listOnly = true; compute = "off"; }
        else if (a == "--no-prevent-clipping") preventClip = false;
        else if (a == "--album-exact") albumExact = true;   // 拼接整张专辑统一门限算专辑响度
        else if (a == "--mode" && i + 1 < argc) { if (!parse_mode(argv[++i], &mode)) { std::fprintf(stderr, "bad --mode\n"); return 2; } }
        else if (a == "--compute" && i + 1 < argc) compute = argv[++i];
        else if (a == "--reference" && i + 1 < argc) ref = std::atof(argv[++i].c_str());
        else if (a == "--") continue;                 // 惯用的"选项到此为止"分隔符
        else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        else inputs.push_back(a);
    }
    if (inputs.empty()) {
        std::fprintf(stderr,
            "rg_probe <文件或目录...> [--mode track|album] [--json] [--compute off|missing|all]\n"
            "         [--reference -18] [--no-prevent-clipping] [--album-exact] [--list]\n");
        return 2;
    }

    std::vector<std::string> files;
    for (const auto& p : inputs) collect(p, &files);
    if (files.empty()) { std::fprintf(stderr, "没有找到音频文件\n"); return 2; }

    std::vector<Row> rows(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        rows[i].path = files[i];
        read_tags(files[i], &rows[i].tr.tags);
        rows[i].tr.path = files[i];
        const TagSet& tg = rows[i].tr.tags;
        rows[i].haveTagTrackGain = tg.has_track_gain() && parse_gain_db(tg.trackGainRaw, &rows[i].tagTrackGain);
        rows[i].haveTagTrackPeak = tg.has_track_peak() && parse_peak_linear(tg.trackPeakRaw, &rows[i].tagTrackPeak);
        rows[i].haveTagAlbumGain = tg.has_album_gain() && parse_gain_db(tg.albumGainRaw, &rows[i].tagAlbumGain);
        rows[i].haveTagAlbumPeak = tg.has_album_peak() && parse_peak_linear(tg.albumPeakRaw, &rows[i].tagAlbumPeak);
    }

    // 计算（缺标签 / 显式全算）
    const bool wantCompute = !listOnly && (compute == "all" || compute == "missing");
    if (wantCompute) {
        for (auto& r : rows) {
            const bool need = (compute == "all") || !r.haveTagTrackGain || !r.haveTagAlbumGain;
            if (!need) continue;
            double lufs = 0.0, pk = 0.0, dur = 0.0;
            if (!measure_file(r.path, &lufs, &pk, &dur)) continue;
            r.computed = true;
            r.tr.meas.valid = true;
            r.tr.meas.lufs = lufs;
            r.tr.meas.peak = pk;
            r.tr.meas.durationSec = dur;
            r.computedTrackGain = gain_from_lufs(lufs, ref);
        }
    }

    // 专辑归组 + 专辑增益（album 模式或需要展示计算值时都要）
    std::vector<Track> tracks(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) tracks[i] = rows[i].tr;

    // --album-exact：对每组把成员拼接成一条信号重测 LUFS（Album Gain 的规范口径）
    std::map<std::string, double> albumOverride;
    if (albumExact && !listOnly) {
        const auto pre = compute_albums(tracks, ref);
        for (const auto& g : pre) {
            if (g.members.size() < 1) continue;
            std::vector<std::string> paths;
            for (size_t i : g.members) paths.push_back(rows[i].path);
            double lufs = 0.0;
            int uc = 0, ur = 0;
            double urd = 0.0;
            if (measure_album_exact(paths, &lufs, &uc, &urd)) { albumOverride[g.key] = lufs; (void)ur; }
        }
    }
    const auto albums = compute_albums(tracks, ref, albumOverride.empty() ? nullptr : &albumOverride);

    Params pr;
    pr.mode = mode;
    pr.referenceLufs = ref;
    pr.preventClipping = preventClip;

    for (size_t i = 0; i < rows.size(); ++i) {
        const std::string k = rows[i].tr.album_key();
        for (const auto& a : albums) {
            if (a.key != k) continue;
            rows[i].albumKey = k;
            rows[i].albumSize = a.members.size();
            rows[i].albumLufs = a.lufs;
            rows[i].albumGainComputed = a.gainDb;
            rows[i].albumComputed = (a.lufs > -299.0);
            break;
        }
        rows[i].dec = decide(rows[i].tr, pr, &albums);
    }

    // ---- 统计 ----
    size_t nTrack = 0, nAlbum = 0, nNone = 0, nBoth = 0;
    std::map<std::string, size_t> byContainer, byKey;
    for (const auto& r : rows) {
        if (r.haveTagTrackGain) ++nTrack;
        if (r.haveTagAlbumGain) ++nAlbum;
        if (!r.haveTagTrackGain && !r.haveTagAlbumGain) ++nNone;
        if (r.haveTagTrackGain && r.haveTagAlbumGain) ++nBoth;
        if (r.tr.tags.ok) byContainer[container_name(r.tr.tags.container)]++;
        const std::string cn(container_name(r.tr.tags.container));
        if (!r.tr.tags.keyTrackGain.empty()) byKey[cn + " :: " + r.tr.tags.keyTrackGain]++;
        if (!r.tr.tags.keyAlbumGain.empty()) byKey[cn + " :: " + r.tr.tags.keyAlbumGain]++;
    }

    if (json) {
        std::printf("{\n  \"mode\": \"%s\",\n  \"referenceLufs\": %.2f,\n", mode_name(mode), ref);
        std::printf("  \"preventClipping\": %s,\n", preventClip ? "true" : "false");
        std::printf("  \"summary\": {\"files\": %zu, \"hasTrackGain\": %zu, \"hasAlbumGain\": %zu, \"hasBoth\": %zu, \"hasNone\": %zu},\n",
                    rows.size(), nTrack, nAlbum, nBoth, nNone);
        std::printf("  \"tracks\": [\n");
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            std::printf("    {\"file\": \"%s\", \"container\": \"%s\", \"tagOk\": %s,\n",
                        json_esc(r.path).c_str(), container_name(r.tr.tags.container), r.tr.tags.ok ? "true" : "false");
            std::printf("     \"keys\": {\"trackGain\": \"%s\", \"trackPeak\": \"%s\", \"albumGain\": \"%s\", \"albumPeak\": \"%s\"},\n",
                        json_esc(r.tr.tags.keyTrackGain).c_str(), json_esc(r.tr.tags.keyTrackPeak).c_str(),
                        json_esc(r.tr.tags.keyAlbumGain).c_str(), json_esc(r.tr.tags.keyAlbumPeak).c_str());
            std::printf("     \"raw\": {\"trackGain\": \"%s\", \"trackPeak\": \"%s\", \"albumGain\": \"%s\", \"albumPeak\": \"%s\"},\n",
                        json_esc(r.tr.tags.trackGainRaw).c_str(), json_esc(r.tr.tags.trackPeakRaw).c_str(),
                        json_esc(r.tr.tags.albumGainRaw).c_str(), json_esc(r.tr.tags.albumPeakRaw).c_str());
            std::printf("     \"parsed\": {\"trackGainDb\": %s, \"trackPeakLinear\": %s, \"trackPeakDbfs\": %s, \"albumGainDb\": %s, \"albumPeakLinear\": %s},\n",
                        r.haveTagTrackGain ? std::to_string(r.tagTrackGain).c_str() : "null",
                        r.haveTagTrackPeak ? std::to_string(r.tagTrackPeak).c_str() : "null",
                        r.haveTagTrackPeak ? std::to_string(linear_to_dbfs(r.tagTrackPeak)).c_str() : "null",
                        r.haveTagAlbumGain ? std::to_string(r.tagAlbumGain).c_str() : "null",
                        r.haveTagAlbumPeak ? std::to_string(r.tagAlbumPeak).c_str() : "null");
            std::printf("     \"computed\": {\"available\": %s, \"lufs\": %s, \"peakLinear\": %s, \"trackGainDb\": %s, \"albumLufs\": %s, \"albumGainDb\": %s},\n",
                        r.computed ? "true" : "false",
                        r.computed ? std::to_string(r.tr.meas.lufs).c_str() : "null",
                        r.computed ? std::to_string(r.tr.meas.peak).c_str() : "null",
                        r.computed ? std::to_string(r.computedTrackGain).c_str() : "null",
                        r.albumComputed ? std::to_string(r.albumLufs).c_str() : "null",
                        r.albumComputed ? std::to_string(r.albumGainComputed).c_str() : "null");
            std::printf("     \"decision\": {\"active\": %s, \"source\": \"%s\", \"requestedDb\": %s, \"appliedDb\": %s, \"clipLimited\": %s, \"fromTag\": %s, \"havePeak\": %s, \"peakDbfs\": %s, \"maxSafeGainDb\": %s, \"note\": \"%s\"}}%s\n",
                        r.dec.active ? "true" : "false", json_esc(r.dec.source).c_str(),
                        std::to_string(r.dec.requestedDb).c_str(), std::to_string(r.dec.appliedDb).c_str(),
                        r.dec.clipLimited ? "true" : "false", r.dec.fromTag ? "true" : "false",
                        r.dec.havePeak ? "true" : "false", std::to_string(r.dec.peakDbfs).c_str(),
                        std::to_string(r.dec.maxSafeGainDb).c_str(), json_esc(r.dec.note).c_str(),
                        (i + 1 < rows.size()) ? "," : "");
        }
        std::printf("  ]\n}\n");
    } else {
        std::printf("mode=%s  reference=%.2f LUFS  preventClipping=%s\n", mode_name(mode), ref, preventClip ? "on" : "off");
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            std::printf("[%zu] %s\n", i + 1, r.path.c_str());
            std::printf("    container=%s tagOk=%d %s%s\n", container_name(r.tr.tags.container), (int)r.tr.tags.ok,
                        r.tr.tags.ok ? "" : r.tr.tags.why.c_str(), r.tr.tags.containerNote.c_str());
            if (!r.tr.tags.keyTrackGain.empty() || !r.tr.tags.keyAlbumGain.empty())
                std::printf("    keys: track_gain=\"%s\" track_peak=\"%s\" album_gain=\"%s\" album_peak=\"%s\"\n",
                            r.tr.tags.keyTrackGain.c_str(), r.tr.tags.keyTrackPeak.c_str(),
                            r.tr.tags.keyAlbumGain.c_str(), r.tr.tags.keyAlbumPeak.c_str());
            std::printf("    raw : track_gain=\"%s\" track_peak=\"%s\" album_gain=\"%s\" album_peak=\"%s\"\n",
                        r.tr.tags.trackGainRaw.c_str(), r.tr.tags.trackPeakRaw.c_str(),
                        r.tr.tags.albumGainRaw.c_str(), r.tr.tags.albumPeakRaw.c_str());
            if (r.haveTagTrackGain || r.haveTagAlbumGain)
                std::printf("    tag : track_gain=%+.3f dB  track_peak=%.6f (%.2f dBFS)  album_gain=%+.3f dB  album_peak=%.6f\n",
                            r.haveTagTrackGain ? r.tagTrackGain : 0.0, r.haveTagTrackPeak ? r.tagTrackPeak : 0.0,
                            r.haveTagTrackPeak ? linear_to_dbfs(r.tagTrackPeak) : -300.0,
                            r.haveTagAlbumGain ? r.tagAlbumGain : 0.0, r.haveTagAlbumPeak ? r.tagAlbumPeak : 0.0);
            if (r.computed)
                std::printf("    meas: LUFS=%.2f  peak=%.6f (%.2f dBFS)  dur=%.1fs  -> computed track_gain=%+.3f dB (ref %.1f)\n",
                            r.tr.meas.lufs, r.tr.meas.peak, linear_to_dbfs(r.tr.meas.peak), r.tr.meas.durationSec,
                            r.computedTrackGain, ref);
            if (r.albumComputed) {
                std::string disp;                                  // 归组键里的 \x1f 是人读不出来的分隔符
                for (char c : r.albumKey) disp += (c == '\x1f') ? std::string(" / ") : std::string(1, c);
                std::printf("    album: key=\"%s\" size=%zu  albumLUFS=%.2f (%s)  album_gain=%+.3f dB\n",
                            disp.c_str(), r.albumSize, r.albumLufs,
                            albumExact && albumOverride.count(r.albumKey) ? "拼接统一门限" : "时长加权近似",
                            r.albumGainComputed);
            }
            std::printf("    decision: active=%d source=%s requested=%+.3f dB applied=%+.3f dB clipLimited=%d peakLimit=%s\n",
                        (int)r.dec.active, r.dec.source.c_str(), r.dec.requestedDb, r.dec.appliedDb,
                        (int)r.dec.clipLimited,
                        r.dec.havePeak ? (std::to_string(r.dec.maxSafeGainDb) + " dB").c_str() : "n/a");
            if (!r.dec.note.empty()) std::printf("    note: %s\n", r.dec.note.c_str());
        }
        std::printf("\nSUMMARY files=%zu hasTrackGain=%zu hasAlbumGain=%zu hasBoth=%zu hasNone=%zu\n",
                    rows.size(), nTrack, nAlbum, nBoth, nNone);
        std::printf("containers:");
        for (const auto& kv : byContainer) std::printf(" %s=%zu", kv.first.c_str(), kv.second);
        std::printf("\nkeys read:");
        for (const auto& kv : byKey) std::printf("\n  %s x%zu", kv.first.c_str(), kv.second);
        std::printf("\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 入口：Windows 上必须走 **宽字符** 命令行。
//   踩坑记录：第一版用 main(int, char**)，PowerShell 传中文路径时 CRT 按 ANSI 代码页转成本地编码，
//   std::filesystem::path(u8string) 收到非法 UTF-8 直接抛异常 ⟹ 进程以 0xC0000409 fail-fast 退出，
//   而且一行输出都没有（stdout 还是块缓冲，崩溃时整块丢掉）。本库文件名大量中日文，
//   所以这里强制 wmain → 逐参数转 UTF-8。
// ---------------------------------------------------------------------------
#ifdef _WIN32
static std::string narrow_utf8(const wchar_t* w, int n) {
    if (!w || n <= 0) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string s((size_t)need, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), need, nullptr, nullptr);
    return s;
}

int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> args;
    args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) args.push_back(narrow_utf8(wargv[i], (int)wcslen(wargv[i])));
    return run_main(args);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
    return run_main(args);
}
#endif

