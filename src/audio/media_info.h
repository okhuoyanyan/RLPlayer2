// media_info.h — 曲目元数据与内嵌封面
//
// 通过 ffprobe 读标签（Title / Artist / Album / Date / Genre），
// 通过 ffmpeg 抽出内嵌封面；没有内嵌封面时退回到同目录的 cover/folder/front 图片。
//
// 封面走磁盘缓存，避免每次请求都重新解码。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "audio/audio_loader.h"
#include "audio/dsd_reader.h"
#include "net/json.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <cstdlib>   // strtod（时长解析：绝不用会抛异常的 std::stod）
#include <cmath>     // std::isfinite（挡 NaN / ±inf）

namespace rta::audio {

struct MediaInfo {
    std::string title, artist, album, date, genre, codec, track;
    std::string composer, work, conductor, performer; // E-15 古典标签支持
    int    sampleRate = 0;
    int    channels = 0;
    int    bits = 0;
    double duration = 0;
    bool   hasCover = false;
};

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ffprobe 的 duration（秒）解析。
//
// 为什么必须自己容错（本函数在播放链路上，抛异常 = 整进程死；把 NaN/inf 带进
// match 的 0.55/0.30/0.15 加权 = 全部分数变 NaN，比不取时长还坏）：
//   * 判不出时长的文件，ffprobe 输出的是字面量 `duration=N/A`（实测 1x1 PNG、
//     0 帧 WAV 都是这个）；空文件/垃圾字节则干脆非 0 退出码（那种情况根本进不到这里）；
//   * 因此绝不能用 std::stod —— 它对 "N/A" 抛 std::invalid_argument。
//
// 统一口径：吃不到数字、尾部有非空白残留（"171.6x" 这种不可信值）、非有限
// （NaN / ±inf，例如 strtod 吃掉 "1e400" 会返回 inf）、<= 0 → 一律返回 0。
// 0 在 match 侧语义正好是「文件侧时长未知 → 中性分」，不会误罚候选。
inline double probe_parse_duration(const std::string& v) {
    const char* s = v.c_str();
    char* end = nullptr;
    const double d = std::strtod(s, &end);
    if (end == s) return 0.0;                        // 一个字符都没吃进去：N/A / 空 / 纯文本
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end != '\0') return 0.0;                   // 尾部残留 → 值不可信
    if (!std::isfinite(d)) return 0.0;               // NaN / ±inf
    if (d <= 0.0) return 0.0;                        // 0 / 负数 → 视为未知
    return d;
}

// 音乐文件时长的合理上限：24 小时。超过它的一定不是「一首很长的曲子」。
inline constexpr double kMediaDurationMaxSec = 86400.0;

// 时长(秒) → 整秒，供 match 的「时长差 ±3s」判据使用（enrich.h 的 fileSec）。
//
// 为什么要在这里挡一次（2026-10-22）：enrich 原来是 (int)(mi.duration + 0.5)。
// 双精度转 int 在 C++ 里是**未定义行为**（越界时 no-op 之外什么都可能发生），
// 本机 clang/x86-64 实测 (int)(1e10 + 0.5) = -4 —— 变成负秒后 match 会静默退回
// 中性分，缺陷不报错、只是信号又没了。触发它只需要一个损坏的头（实测 RF64
// 把 dataSize 声明成 1e12，ffprobe 就照实输出 duration=1000000000000）。
//
// 超上限时的选择：返回 0（= 未知）而不是钳到 86400。
// 理由：钳值会把「读不懂」变成一个**确定的错值**，match 侧据此给出 far=0 分，
// 把本来正确的候选判死；返回 0 则与「时长缺失」同义，走中性 0.5，宁缺毋错
// （与 match.h 中性降级的设计一致）。
inline int media_duration_sec(const double seconds) {
    if (!std::isfinite(seconds)) return 0;              // NaN / ±inf
    if (seconds <= 0.0) return 0;                       // 未知
    if (seconds > kMediaDurationMaxSec) return 0;       // 24h 以上视为头损坏 → 未知
    const double r = seconds + 0.5;                     // 四舍五入到最近的整秒
    if (r >= 2147483647.0) return 0;                    // 兜底：理论上不可达（上限已挡）
    return (int)r;
}

inline bool is_dsd_path(const std::string& path) {
    const size_t d = path.find_last_of('.');
    if (d == std::string::npos) return false;
    std::string ext = path.substr(d);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".dsf" || ext == ".dff";
}

// 纯函数：合并 format.tags 与 stream.tags（E-4）。
// 规则：
// 1. 键名大小写不敏感（统一转小写；剥除 tag: / TAG: 前缀）；
// 2. format 优先：同名键若 format 有非空值，以 format 为准；
// 3. stream 补缺：若 format 缺失该键或值为空/空白，用 stream 的非空值补全；
// 4. 空值修剪：去首尾空白，两边皆空/空白的键不注入结果。
inline std::map<std::string, std::string> merge_tags(
    const std::map<std::string, std::string>& format_tags,
    const std::map<std::string, std::string>& stream_tags) {
    std::map<std::string, std::string> res;

    auto norm_key = [](const std::string& raw) {
        std::string k = trim(raw);
        if (k.rfind("tag:", 0) == 0 || k.rfind("TAG:", 0) == 0) k = k.substr(4);
        for (auto& c : k) c = (char)std::tolower((unsigned char)c);
        return k;
    };

    // 1. 先把 stream_tags 写入
    for (const auto& [rk, rv] : stream_tags) {
        std::string k = norm_key(rk);
        std::string v = trim(rv);
        if (!k.empty() && !v.empty()) {
            res[k] = v;
        }
    }

    // 2. format_tags 优先覆盖（若 format 值为非空，则覆盖 stream；若 format 为空，保留 stream 补缺）
    for (const auto& [rk, rv] : format_tags) {
        std::string k = norm_key(rk);
        std::string v = trim(rv);
        if (k.empty()) continue;
        if (!v.empty()) {
            res[k] = v;
        }
    }

    return res;
}

inline void apply_tags_to_media_info(MediaInfo& mi, const std::map<std::string, std::string>& tags) {
    auto find_val = [&](const std::string& k) -> std::string {
        auto it = tags.find(k);
        return (it != tags.end()) ? it->second : std::string();
    };

    std::string v;
    if (!(v = find_val("title")).empty()) mi.title = v;
    if (!(v = find_val("artist")).empty()) mi.artist = v;
    if (!(v = find_val("album")).empty()) mi.album = v;
    if (!(v = find_val("date")).empty()) mi.date = v;
    if (!(v = find_val("genre")).empty()) mi.genre = v;
    if (!(v = find_val("track")).empty()) mi.track = v;
    if (!(v = find_val("composer")).empty()) mi.composer = v;
    if (!(v = find_val("work")).empty()) mi.work = v;
    if (!(v = find_val("conductor")).empty()) mi.conductor = v;
    if (!(v = find_val("performer")).empty()) mi.performer = v;
    else if (!(v = find_val("ensemble")).empty()) mi.performer = v;
    else if (!(v = find_val("soloist")).empty()) mi.performer = v;
}

// 纯函数：解析 ffprobe -of json 格式的输出（E-4）
// 支持从 format.tags 与首个音频流（codec_type=audio）的 tags 提取并合并元数据，并提取流信息与时长。
inline MediaInfo probe_parse_json(const std::string& raw_text, bool dsdHeaderOk = false, const MediaInfo& initial = MediaInfo()) {
    MediaInfo mi = initial;
    // 容错：run_hidden 把 stderr 与 stdout 合并，ffprobe 的警告（如 attached picture 警告）
    // 会出现在 JSON 之前；剥离首个 '{' 之前和末尾 '}' 之后的日志字符。
    const size_t start = raw_text.find('{');
    const size_t end = raw_text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start) return mi;
    const std::string json_text = raw_text.substr(start, end - start + 1);
    const rta::net::Json root = rta::net::json_parse(json_text);
    if (root.type != rta::net::Json::Obj) return mi;

    std::map<std::string, std::string> format_tags;
    std::map<std::string, std::string> stream_tags;

    // 1. format 节点：duration 与 format_tags
    if (root.has("format") && root["format"].type == rta::net::Json::Obj) {
        const auto& fmt = root["format"];
        if (fmt.has("duration") && !dsdHeaderOk) {
            mi.duration = probe_parse_duration(fmt["duration"].s());
        }
        if (fmt.has("tags") && fmt["tags"].type == rta::net::Json::Obj) {
            for (const auto& [k, v] : fmt["tags"].obj) {
                format_tags[k] = v.s();
            }
        }
    }

    // 2. streams 节点：找到首个 audio 流
    if (root.has("streams") && root["streams"].type == rta::net::Json::Arr) {
        bool foundAudio = false;
        for (size_t i = 0; i < root["streams"].size(); ++i) {
            const auto& stm = root["streams"].at(i);
            if (stm.type != rta::net::Json::Obj) continue;
            const std::string ctype = stm.has("codec_type") ? stm["codec_type"].s() : "";
            if (ctype == "audio") {
                if (!foundAudio) {
                    foundAudio = true;
                    if (mi.codec.empty() && stm.has("codec_name")) {
                        mi.codec = stm["codec_name"].s();
                    }
                    if (!dsdHeaderOk) {
                        if (stm.has("sample_rate")) {
                            mi.sampleRate = atoi(stm["sample_rate"].s().c_str());
                        }
                        if (stm.has("channels")) {
                            mi.channels = (int)stm["channels"].n();
                            if (mi.channels <= 0 && stm["channels"].type == rta::net::Json::Str) {
                                mi.channels = atoi(stm["channels"].s().c_str());
                            }
                        }
                        if (stm.has("bits_per_raw_sample")) {
                            const std::string b = stm["bits_per_raw_sample"].s();
                            if (b != "N/A" && !b.empty()) {
                                mi.bits = atoi(b.c_str());
                            }
                        }
                    }
                    if (stm.has("tags") && stm["tags"].type == rta::net::Json::Obj) {
                        for (const auto& [k, v] : stm["tags"].obj) {
                            stream_tags[k] = v.s();
                        }
                    }
                }
            }
        }
    }

    // 3. 合并标签（format 优先，stream 补缺）
    const auto merged = merge_tags(format_tags, stream_tags);
    apply_tags_to_media_info(mi, merged);

    if (mi.bits <= 0) mi.bits = dsdHeaderOk ? 1 : 16;
    return mi;
}

inline MediaInfo probe_media(const std::string& file) {
    MediaInfo mi;

    // E-16：对 .dsf/.dff 优先用 dsd_reader 解析 header 计算 duration
    // DSF: fmt 块的 sampleCount(每声道 1-bit 样本数) ÷ samplingFrequency
    // DFF: data 块字节数 ÷ 声道数 × 8 ÷ FS
    // 若 header 解析失败再回退 ffprobe，行为保持现有回退链。
    bool dsdHeaderOk = false;
    if (is_dsd_path(file)) {
        try {
            const DsdInfo di = probe_dsd(file);
            const double s = di.seconds();
            if (std::isfinite(s) && s > 0.0) {
                mi.duration = s;
                dsdHeaderOk = true;
            }
            if (di.rate > 0) mi.sampleRate = (int)di.rate;
            if (di.channels > 0) mi.channels = di.channels;
            mi.bits = 1;
            mi.codec = (di.container == "dsf" ? "dsd_lsbf" : "dsd_msbf");
        } catch (...) {
            // header 解析失败，回退 ffprobe
            dsdHeaderOk = false;
        }
    }

    const std::string ffprobe = find_tool("ffprobe");
    if (ffprobe.empty()) return mi;

    // E-4：合并 format.tags 与首个音频流（codec_type=audio）的 tags（format 优先，stream 补缺）
    // 单次 ffprobe 同时输出 format 与 stream 元数据及参数，-of json 统一解析。
    std::string out;
    const std::wstring cmd = L"\"" + widen(ffprobe) +
        L"\" -v error -show_entries format=duration:format_tags:stream=codec_name,codec_type,sample_rate,channels,bits_per_raw_sample:stream_tags "
        L"-of json \"" + widen(file) + L"\"";
    if (run_hidden(cmd, &out) == 0 && !out.empty()) {
        mi = probe_parse_json(out, dsdHeaderOk, mi);
    }
    if (mi.bits <= 0) mi.bits = dsdHeaderOk ? 1 : 16;
    return mi;
}

// ---------------- 封面 ----------------
struct CoverCache {
    std::mutex mtx;
    std::map<std::string, std::vector<unsigned char>> data;
    std::map<std::string, std::string>               mime;
};
inline CoverCache& cover_cache() { static CoverCache c; return c; }

inline std::string temp_path(const wchar_t* ext) {
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
    wchar_t name[MAX_PATH] = {0};
    GetTempFileNameW(dir, L"rta", 0, name);
    std::wstring w(name);
    w += ext;
    const int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), m, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// 返回封面字节；同时把 mime 写进 outMime
inline std::vector<unsigned char> get_cover(const std::string& file, std::string& outMime) {
    auto& cc = cover_cache();
    {
        std::lock_guard<std::mutex> lk(cc.mtx);
        auto it = cc.data.find(file);
        if (it != cc.data.end()) { outMime = cc.mime[file]; return it->second; }
    }
    std::vector<unsigned char> bytes;
    outMime = "image/jpeg";

    // 1) 同目录的 cover / folder / front
    try {
        std::filesystem::path p = std::filesystem::u8path(file);
        const auto dir = p.parent_path();
        const char* names[] = { "cover", "folder", "front", "Cover", "Folder" };
        const char* exts[]  = { ".jpg", ".jpeg", ".png", ".JPG", ".PNG" };
        for (const auto& n : names) {
            for (const auto& e : exts) {
                auto cand = dir / (std::string(n) + e);
                std::error_code ec;
                if (std::filesystem::exists(cand, ec)) {
                    std::ifstream f(cand, std::ios::binary);
                    if (f) {
                        bytes.assign(std::istreambuf_iterator<char>(f), {});
                        outMime = (e[1] == 'p' || e[1] == 'P') ? "image/png" : "image/jpeg";
                        goto done;
                    }
                }
            }
        }
    } catch (...) {}

    // 2) 内嵌封面
    {
        const std::string ffmpeg = find_tool("ffmpeg");
        if (!ffmpeg.empty()) {
            const std::string tmp = temp_path(L".png");
            const std::wstring cmd = L"\"" + widen(ffmpeg) + L"\" -v error -y -i \"" + widen(file) +
                L"\" -map 0:v:0 -c:v png -f image2 \"" + widen(tmp) + L"\"";
            run_hidden(cmd, nullptr);
            std::ifstream f(std::filesystem::u8path(tmp), std::ios::binary);
            if (f) {
                std::vector<unsigned char> b{ std::istreambuf_iterator<char>(f), {} };
                f.close();
                if (b.size() > 64) { bytes = std::move(b); outMime = "image/png"; }
            }
            DeleteFileW(widen(tmp).c_str());
            if (!bytes.empty()) goto done;
        }
    }
    // 3) 都没有：返回 1x1 透明 PNG
    {
        static const unsigned char px[] = {
            0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0,0,0,0x0D,0x49,0x48,0x44,0x52,
            0,0,0,1,0,0,0,1,8,6,0,0,0,0x1F,0x15,0xC4,0x89,0,0,0,0x0A,0x49,0x44,0x41,
            0x54,0x78,0x9C,0x63,0,1,0,0,5,0,1,0x0D,0x0A,0x2D,0xB4,0,0,0,0,0x49,
            0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
        };
        bytes.assign(px, px + sizeof(px));
        outMime = "image/png";
    }

done:
    {
        std::lock_guard<std::mutex> lk(cc.mtx);
        cc.data[file] = bytes;
        cc.mime[file] = outMime;
    }
    return bytes;
}

} // namespace rta::audio
