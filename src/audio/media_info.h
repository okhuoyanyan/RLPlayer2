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

inline MediaInfo probe_media(const std::string& file) {
    MediaInfo mi;
    const std::string ffprobe = find_tool("ffprobe");
    if (ffprobe.empty()) return mi;

    // 格式级标签 + 格式级时长
    //
    // 为什么要在同一条命令里要 format=duration（2026-10-22 缺陷修复）：
    // 原来只问 format_tags，MediaInfo::duration 恒为 0 → enrich.h 的 fileSec 恒 0 →
    // match.h 的 mf_duration 一直走「任一侧缺失 → 0.5 中性」，文档里写的「时长差 ±3s
    // 强信号」从 P1 起从未生效。duration 是 format 级字段（不是 TAG: 前缀的标签），
    // 所以走同一个解析循环、按 k=="duration" 单独认，不跟 tag:xxx 混。
    {
        std::string out;
        const std::wstring cmd = L"\"" + widen(ffprobe) +
            L"\" -v error -show_entries format=duration:format_tags=title,artist,album,date,genre "
            L"-of default=nw=1 \"" + widen(file) + L"\"";
        if (run_hidden(cmd, &out) == 0) {
            std::string line;
            for (size_t i = 0; i <= out.size(); ++i) {
                if (i == out.size() || out[i] == '\n') {
                    const size_t e = line.find('=');
                    if (e != std::string::npos) {
                        std::string k = trim(line.substr(0, e));
                        const std::string v = trim(line.substr(e + 1));
                        // ffprobe 保留原始大小写：有的文件是 TAG:Title，有的是 TAG:title。
                        // 统一转小写再比较，否则会漏掉一半的库（本轮实测踩到）。
                        for (auto& c : k) c = (char)std::tolower((unsigned char)c);
                        if (k == "tag:title"  || k == "title")  mi.title = v;
                        else if (k == "tag:artist" || k == "artist") mi.artist = v;
                        else if (k == "tag:album"  || k == "album")  mi.album = v;
                        else if (k == "tag:date"   || k == "date")   mi.date = v;
                        else if (k == "tag:genre"  || k == "genre")  mi.genre = v;
                        else if (k == "tag:track"  || k == "track")  mi.track = v;
                        // duration 是 format 级字段，输出行形如 "duration=171.692290"，
                        // 【没有】TAG: 前缀；它恰好与 tag 键共处一个 k=v 循环，所以在此认。
                        // 解析失败一律保持 0（未知），绝不把 NaN/inf 传下去。
                        else if (k == "duration")                    mi.duration = probe_parse_duration(v);
                    }
                    line.clear();
                } else if (line.size() < 4096) line += out[i];
            }
        }
    }
    // 流级参数
    {
        std::string out;
        const std::wstring cmd = L"\"" + widen(ffprobe) +
            L"\" -v error -select_streams a:0 -show_entries "
            L"stream=codec_name,sample_rate,channels,bits_per_raw_sample "
            L"-of default=nw=1 \"" + widen(file) + L"\"";
        if (run_hidden(cmd, &out) == 0) {
            std::string line;
            for (size_t i = 0; i <= out.size(); ++i) {
                if (i == out.size() || out[i] == '\n') {
                    const size_t e = line.find('=');
                    if (e != std::string::npos) {
                        const std::string k = trim(line.substr(0, e));
                        const std::string v = trim(line.substr(e + 1));
                        if (k == "codec_name") mi.codec = v;
                        else if (k == "sample_rate") mi.sampleRate = atoi(v.c_str());
                        else if (k == "channels") mi.channels = atoi(v.c_str());
                        else if (k == "bits_per_raw_sample") mi.bits = atoi(v.c_str());
                    }
                    line.clear();
                } else if (line.size() < 512) line += out[i];
            }
        }
    }
    if (mi.bits <= 0) mi.bits = 16;
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
