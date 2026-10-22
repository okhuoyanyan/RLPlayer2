// test_ffmpeg_matrix.cpp — 精简版 ffmpeg/ffprobe 的音频格式矩阵门禁（P1：裁剪 ffmpeg）
//
// 为什么需要这个测试（铁律 3：能证明「要证明的事」）：
//   P1 把内置 ffmpeg 从 gyan full（212+211.8 MB）换成自建精简版（12+11.8 MB）。
//   体积变小本身没有意义 —— 必须证明【产品自己的解码路径】对每一种音频格式仍然
//   给出有效结果（rate/channels/frames/电平），且封面、标签写回(remux)这些依赖
//   ffmpeg 的功能没被裁掉。所以：
//     * 解码断言走 rta::audio::read_any()（不是直接跑 ffmpeg CLI）——这才是产品路径；
//     * 样本由 ffmpeg 现场合成（−60 dBFS，**绝不发声**：本测试不打开任何音频设备）；
//     * 「用的是精简版」不是靠 PATH 碰运气：父进程把精简版 ffmpeg/ffprobe 与
//       测试自身复制到临时目录，再在该目录起子进程 —— find_tool() 先看 exe 同目录，
//       所以子进程必定用精简版（子进程会打印实际解析到的路径，可核对）。
//
// ⚠️ 未覆盖（铁律 4，如实标注）：
//   * ape / mpc7 / mpc8 / tak / shorten / dst 等**无法现场合成**：ffmpeg 只有它们的
//     解码器、没有编码器（实测 ffmpeg 9.0：encoders 里无 ape/mpc/tak/shorten）。
//     这些格式在本测试里只断言「解码器存在」（白名单），没有真实样本解码断言。
//     白名单见 tools/ffmpeg_lite_required.txt（与出包守卫共用同一份，避免两份漂移）。
//   * DSF/DFF 样本由本测试自己按 DSF/DSDIFF 规范合成（ffmpeg 9.0 无 DSD 编码器），
//     所以它们验证的是「容器 + 1-bit 流」的读取，不是某个真实录音母带的特征。
//
// 用法：test_ffmpeg_matrix            父进程：准备环境 + 起子进程
//       test_ffmpeg_matrix --child DIR 子进程：在 DIR 里跑全部断言
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#endif
#include "audio/audio_loader.h"
#include "audio/media_info.h"
#include "meta/writeback.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif

namespace fs = std::filesystem;
using rta::audio::AudioBuffer;

namespace {

constexpr double kPi = 3.14159265358979323846;

int g_fail = 0;
int g_pass = 0;

void ok(const std::string& what)   { ++g_pass; std::printf("[PASS] %s\n", what.c_str()); }
void bad(const std::string& what)  { ++g_fail; std::printf("[FAIL] %s\n", what.c_str()); }
void skip(const std::string& what) { std::printf("[SKIP] %s\n", what.c_str()); }
void info(const std::string& what) { std::printf("[INFO] %s\n", what.c_str()); }

std::string exe_dir() {
    wchar_t p[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (n == 0) return "";
    std::wstring s(p, n);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return "";
    return rta::audio::wide_to_utf8(s.substr(0, slash));
}

// 只查 PATH（不查 exe 同目录）——用来找"合成样本用的"系统 ffmpeg（full 版有编码器），
// 与产品用的 find_tool() 刻意区分，避免精简版把自己当合成器（它没有编码器）。
std::string path_only(const char* name) {
    wchar_t buf[4096];
    const std::wstring w = rta::audio::widen(name);
    const DWORD n = SearchPathW(nullptr, w.c_str(), L".exe", 4096, buf, nullptr);
    if (n == 0 || n >= 4096) return "";
    return rta::audio::wide_to_utf8(std::wstring(buf, n));
}

int run(const std::wstring& cmd, std::string* out = nullptr) {
    std::string buf;
    return rta::audio::run_hidden(cmd, out ? out : &buf);
}

bool exists(const std::string& p) { return fs::exists(fs::u8path(p)); }
long long fsize(const std::string& p) {
    std::error_code ec;
    const auto s = fs::file_size(fs::u8path(p), ec);
    return ec ? -1 : (long long)s;
}

// 峰值（相对满幅）。用来抓「解出来全是 0」这类假通过。
double peak(const AudioBuffer& b) {
    double m = 0.0;
    for (size_t i = 0; i < b.frames(); ++i)
        for (int c = 0; c < b.channels; ++c) m = std::max(m, std::fabs(b.sample(i, c)));
    return m;
}

double rms(const AudioBuffer& b) {
    double acc = 0.0;
    const size_t n = b.frames();
    if (n == 0) return 0.0;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < b.channels; ++c) { const double v = b.sample(i, c); acc += v * v; }
    return std::sqrt(acc / (double)(n * (size_t)b.channels));
}

std::string dbfs(double x) {
    char buf[64];
    if (x <= 0.0) { std::snprintf(buf, sizeof buf, "-inf dBFS"); return buf; }
    std::snprintf(buf, sizeof buf, "%.1f dBFS", 20.0 * std::log10(x));
    return buf;
}

// ---------------- 现场合成 ----------------

// DSF/DSDIFF 合成：二阶 sigma-delta 把 −60 dBFS 1 kHz 正弦变成 1-bit 流。
// 位序：内部按 MSB-first 打包；DSF 文件是 LSB-first，所以写文件前按字节反位。
std::vector<std::vector<uint8_t>> make_dsd(int64_t bitsPerCh, int ch, uint32_t rate, double amp) {
    std::vector<std::vector<uint8_t>> out((size_t)ch, std::vector<uint8_t>((size_t)((bitsPerCh + 7) / 8), 0));
    for (int c = 0; c < ch; ++c) {
        double i1 = 0.0, i2 = 0.0, y = 1.0;
        for (int64_t n = 0; n < bitsPerCh; ++n) {
            const double in = amp * std::sin(2.0 * kPi * 1000.0 * (double)n / (double)rate +
                                             (c == 0 ? 0.0 : 0.35));
            y = (i2 >= 0.0) ? 1.0 : -1.0;
            i1 += in - y;
            i2 += i1 - y;
            if (i1 > 4.0) i1 = 4.0; if (i1 < -4.0) i1 = -4.0;   // 防积分器发散
            if (i2 > 4.0) i2 = 4.0; if (i2 < -4.0) i2 = -4.0;
            if (y > 0.0) out[(size_t)c][(size_t)(n >> 3)] |= (uint8_t)(1u << (7 - (n & 7)));
        }
    }
    return out;
}

void put_le32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); }
void put_le64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 0; i < 8; ++i) v.push_back((uint8_t)(x >> (8 * i))); }
void put_be16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void put_be32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back((uint8_t)(x >> (8 * i))); }
void put_be64(std::vector<uint8_t>& v, uint64_t x) { for (int i = 7; i >= 0; --i) v.push_back((uint8_t)(x >> (8 * i))); }
void put_tag(std::vector<uint8_t>& v, const char* t) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)t[i]); }

uint8_t reverse_bits(uint8_t b) {
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& v) {
    std::ofstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    f.write((const char*)v.data(), (std::streamsize)v.size());
    return (bool)f;
}

// DSF：28 字节头 + fmt(52) + data；每声道按 blockSize 分块交错（末块补齐整块）。
bool write_dsf(const std::string& path, uint32_t rate, int ch, double seconds, double amp) {
    const int64_t bits = (int64_t)((double)rate * seconds);
    auto data = make_dsd(bits, ch, rate, amp);
    const uint32_t blockSize = 4096;
    const uint64_t needBytes = (uint64_t)((bits + 7) / 8);
    const uint64_t blocks = (needBytes + blockSize - 1) / blockSize;
    std::vector<uint8_t> body;
    for (uint64_t b = 0; b < blocks; ++b)
        for (int c = 0; c < ch; ++c)
            for (uint32_t i = 0; i < blockSize; ++i) {
                const uint64_t idx = b * blockSize + i;
                uint8_t byte = (idx < needBytes) ? data[(size_t)c][(size_t)idx] : 0;
                body.push_back(reverse_bits(byte));       // 文件内 LSB-first
            }
    std::vector<uint8_t> f;
    put_tag(f, "DSD ");
    put_le64(f, 28);
    put_le64(f, 0);                                        // 文件总长（读者不校验，稍后回填）
    put_le64(f, 0);                                        // 元数据指针
    put_tag(f, "fmt ");
    put_le64(f, 52);
    put_le32(f, 1);                                        // format version = 1（DSF 规范：body 第 1 个字段）
    put_le32(f, 0);                                        // format id = 0（未压缩 DSD）
    put_le32(f, 2);                                        // channel type = 2（stereo）
    put_le32(f, (uint32_t)ch);
    put_le32(f, rate);
    put_le32(f, 1);                                        // bits per sample
    put_le64(f, (uint64_t)bits);
    put_le32(f, blockSize);
    put_le32(f, 0);                                        // reserved
    put_tag(f, "data");
    put_le64(f, 12 + body.size());
    f.insert(f.end(), body.begin(), body.end());
    const uint64_t total = f.size();
    for (int i = 0; i < 8; ++i) f[(size_t)(12 + i)] = (uint8_t)(total >> (8 * i));
    return write_file(path, f);
}

// DFF(DSDIFF)：FRM8("DSD ") → FVER → PROP(SND: FS/CHNL/CMPR) → DSD(每声道连续)
bool write_dff(const std::string& path, uint32_t rate, int ch, double seconds, double amp) {
    const int64_t bits = (int64_t)((double)rate * seconds);
    auto data = make_dsd(bits, ch, rate, amp);
    const uint64_t perCh = (uint64_t)((bits + 7) / 8);

    std::vector<uint8_t> snd;
    put_tag(snd, "SND ");
    put_tag(snd, "FS  "); put_be64(snd, 4); put_be32(snd, rate);
    // DSDIFF 的 CHNL：声道数是【2 字节】大端（不是 4 字节）——写错会被 ffmpeg 判成
    // "compression is not supported"/Invalid data（2026-10-22 实测踩到）。
    put_tag(snd, "CHNL"); put_be64(snd, 2 + 4 * (uint64_t)ch); put_be16(snd, (uint16_t)ch);
    const char* ids[2] = {"SLFT", "SRGT"};
    for (int c = 0; c < ch; ++c) put_tag(snd, ids[c & 1]);
    put_tag(snd, "CMPR"); put_be64(snd, 4 + 1 + 3); put_tag(snd, "DSD ");
    snd.push_back(14); snd.push_back(0); snd.push_back(0); snd.push_back(0);   // 名称长度 + "not compressed"

    std::vector<uint8_t> body;
    put_tag(body, "FVER"); put_be64(body, 4); put_be32(body, 0x01050000u);
    put_tag(body, "PROP"); put_be64(body, snd.size()); body.insert(body.end(), snd.begin(), snd.end());
    if (snd.size() & 1) body.push_back(0);
    put_tag(body, "DSD "); put_be64(body, perCh * (uint64_t)ch);
    for (int c = 0; c < ch; ++c) body.insert(body.end(), data[(size_t)c].begin(), data[(size_t)c].end());
    if (body.size() & 1) body.push_back(0);

    std::vector<uint8_t> f;
    put_tag(f, "FRM8"); put_be64(f, 4 + body.size()); put_tag(f, "DSD ");
    f.insert(f.end(), body.begin(), body.end());
    return write_file(path, f);
}

// ---------------- 白名单 ----------------
std::vector<std::string> read_required_list() {
    std::vector<std::string> out;
#ifdef RTA_SOURCE_DIR
    const std::string p = std::string(RTA_SOURCE_DIR) + "/tools/ffmpeg_lite_required.txt";
    std::ifstream f(fs::u8path(p));
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        out.push_back(line);
    }
#endif
    return out;
}

// ffmpeg -decoders 的音频解码器名字集合
std::vector<std::string> ffmpeg_audio_decoders(const std::string& ffmpeg) {
    std::string out;
    if (run(L"\"" + rta::audio::widen(ffmpeg) + L"\" -hide_banner -decoders", &out) != 0) return {};
    std::vector<std::string> names;
    size_t i = 0;
    while (i < out.size()) {
        size_t e = out.find('\n', i);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(i, e - i);
        i = e + 1;
        // 形如 " A....D aac                  AAC (...)"；flags 首字符 A = audio
        if (line.size() > 3 && line[0] == ' ' && line[1] == 'A') {
            size_t p = line.find_first_not_of(' ', 2);
            if (p != std::string::npos) {
                // 跳过 flags 剩余部分
                while (p < line.size() && line[p] != ' ') ++p;
                while (p < line.size() && line[p] == ' ') ++p;
                size_t q2 = line.find_first_of(" \t", p);
                if (p < line.size()) {
                    const std::string nm = line.substr(p, (q2 == std::string::npos ? line.size() : q2) - p);
                    // 图例行 "A..... = Audio" 会解析出 "="，必须滤掉（否则计数多 1）
                    const bool name_like = !nm.empty() &&
                        nm.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.") == std::string::npos;
                    if (name_like) names.push_back(nm);
                }
            }
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

// ---------------- 子进程：全部断言 ----------------
struct Case { const char* label; const char* file; };

int child_main(const std::string& dir) {
    const std::string ffmpeg = rta::audio::find_tool("ffmpeg");
    const std::string ffprobe = rta::audio::find_tool("ffprobe");
    info("find_tool(ffmpeg)  = " + (ffmpeg.empty() ? "<未找到>" : ffmpeg));
    info("find_tool(ffprobe) = " + (ffprobe.empty() ? "<未找到>" : ffprobe));
    if (ffmpeg.empty() || ffprobe.empty()) {
        bad("找不到 ffmpeg/ffprobe（子进程应运行在含精简版的目录里）");
        return 1;
    }

    // --- 1. 解码器白名单（与出包守卫共用同一份列表） ---
    {
        const auto req = read_required_list();
        const auto have = ffmpeg_audio_decoders(ffmpeg);
        if (req.empty()) {
            bad("读不到 tools/ffmpeg_lite_required.txt（RTA_SOURCE_DIR 未定义？）");
        } else if (have.empty()) {
            bad("ffmpeg -decoders 没有输出（精简版不可执行？）");
        } else {
            std::vector<std::string> miss;
            for (const auto& n : req)
                if (!std::binary_search(have.begin(), have.end(), n)) miss.push_back(n);
            if (miss.empty()) {
                ok("解码器白名单 " + std::to_string(req.size()) + " 项齐全（精简版共 " +
                   std::to_string(have.size()) + " 个音频解码器）");
            } else {
                std::string s;
                for (const auto& m : miss) s += m + " ";
                bad("精简版缺必需解码器：" + s);
            }
        }
    }

    // --- 2. 格式矩阵：走产品自己的 read_any ---
    const std::vector<Case> cases = {
        {"flac",    "m.flac"}, {"mp3", "m.mp3"}, {"aac(m4a)", "m_aac.m4a"},
        {"alac(m4a)", "m_alac.m4a"}, {"ogg-vorbis", "m.ogg"}, {"opus", "m.opus"},
        {"wav",     "m.wav"},  {"wma",  "m.wma"}, {"tta", "m.tta"},
        {"wavpack", "m.wv"},   {"dsf",  "m.dsf"}, {"dff", "m.dff"},
    };
    for (const auto& c : cases) {
        const std::string path = dir + "/" + c.file;
        if (!exists(path)) { skip(std::string(c.label) + "：样本不存在（合成阶段跳过）"); continue; }
        rta::audio::LoadNote note;
        try {
            const AudioBuffer b = rta::audio::read_any(path, &note);
            const size_t frames = b.frames();
            const double pk = peak(b);
            const double rmsV = rms(b);
            char detail[640];
            std::snprintf(detail, sizeof detail,
                          "%s: rate=%u ch=%d bits=%d frames=%zu rms=%.5f (%s) peak=%.4f (%s) note=%s",
                          c.label, b.rate, b.channels, b.bits, frames, rmsV, dbfs(rmsV).c_str(),
                          pk, dbfs(pk).c_str(),
                          note.text.empty() ? "-" : note.text.c_str());
            const bool frames_ok = frames > (size_t)(b.rate / 10);          // 至少 0.1 s
            // −60 dBFS 正弦的 RMS = 7.07e-4。留 ±10 dB 余量容纳有损编码与 DSD 噪声整形；
            // 下界防"解出全 0"，上界防"解出噪声/错位数据"。DSD 的 out-of-band 噪声
            // 主要抬高峰值，所以判据用 RMS（峰值只打印，不判定）。
            const bool level_ok = rmsV > 2e-4 && rmsV < 4e-3;
            if (b.rate >= 8000 && b.channels >= 1 && b.channels <= 8 && frames_ok && level_ok)
                ok(detail);
            else
                bad(std::string(detail) + (frames_ok ? "" : " [帧数太少]") + (level_ok ? "" : " [电平异常]"));
        } catch (const std::exception& e) {
            bad(std::string(c.label) + "：read_any 抛异常：" + e.what());
        }
    }

    // --- 2b. DSD 回退路径：read_any 对 .dsf/.dff 先走原生读取，读不了才回退 ffmpeg。
    //     所以这里直接用精简版 ffmpeg 解一次，证明"回退解码器"也在包里（不是白列） ---
    for (const char* f : {"m.dsf", "m.dff"}) {
        const std::string src = dir + "/" + f;
        if (!exists(src)) { skip(std::string("DSD 回退解码 ") + f + "：样本缺失"); continue; }
        const std::string out = dir + "/dsdfallback.wav";
        std::string log;
        const int rc = run(L"\"" + rta::audio::widen(ffmpeg) + L"\" -hide_banner -v error -y -i \"" +
                           rta::audio::widen(src) + L"\" -vn -c:a pcm_s24le -f wav \"" +
                           rta::audio::widen(out) + L"\"", &log);
        if (rc == 0 && fsize(out) > 1000) ok(std::string("精简版 ffmpeg 回退解码 ") + f);
        else bad(std::string("精简版 ffmpeg 回退解码 ") + f + "：rc=" + std::to_string(rc) + " " + log.substr(0, 160));
    }

    // --- 3. 标签写回（remux，-c copy）用到的 8 个 muxer ---
    {
        struct Remux { const char* ext; const char* muxer; const char* src; };
        const Remux rm[] = {
            {".flac", "flac", "m.flac"}, {".mp3", "mp3", "m.mp3"},
            {".ogg",  "ogg",  "m.ogg"},  {".opus", "opus", "m.opus"},
            {".m4a",  "ipod", "m_aac.m4a"}, {".wma", "asf", "m.wma"},
            {".wav",  "wav",  "m.wav"},  {".wv", "wv", "m.wv"},
        };
        for (const auto& r : rm) {
            const std::string src = dir + "/" + r.src;
            if (!exists(src)) { skip(std::string("remux ") + r.muxer + "：源样本缺失"); continue; }
            const std::string dst = dir + "/rm" + r.ext + ".rtatmp";
            const bool vorbis = rta::meta::writeback_vorbis_family(r.ext);
            std::vector<rta::meta::WbField> kvs;
            kvs.push_back(rta::meta::WbField{"title", vorbis ? "TITLE" : "title",
                                             std::string("matrix-") + r.muxer});
            std::string err;
            if (rta::meta::writeback_remux(ffmpeg, src, dst, r.muxer, std::string(r.ext) == ".mp3", kvs, err) &&
                fsize(dst) > 100)
                ok(std::string("remux -f ") + r.muxer + "（writeback 用）");
            else
                bad(std::string("remux -f ") + r.muxer + "：" + (err.empty() ? "产物过小/失败" : err));
        }
        // 顺带用 ffprobe 读回一个 remux 产物，证明标签真的写进去了
        const std::string probeFile = dir + "/rm.flac.rtatmp";
        if (exists(probeFile)) {
            std::string out;
            const std::wstring cmd = L"\"" + rta::audio::widen(ffprobe) +
                L"\" -v error -show_entries format_tags=TITLE -of default=nw=1:nk=1 \"" +
                rta::audio::widen(probeFile) + L"\"";
            if (run(cmd, &out) == 0 && out.find("matrix-flac") != std::string::npos)
                ok("ffprobe 读回 remux 后的标签（TITLE=matrix-flac）");
            else
                bad("ffprobe 读不回 remux 后的标签：" + out.substr(0, 120));
        }
    }

    // --- 3b. PCM 逐字节门禁（writeback.h 的核心判据：remux 前后解码必须完全一致） ---
    {
        struct Pr { const char* src; const char* dst; };
        const Pr pr[] = {
            {"m.flac", "rm.flac.rtatmp"}, {"m.mp3", "rm.mp3.rtatmp"},
            {"m.ogg",  "rm.ogg.rtatmp"},  {"m.opus", "rm.opus.rtatmp"},
            {"m_aac.m4a", "rm.m4a.rtatmp"}, {"m.wma", "rm.wma.rtatmp"},
            {"m.wav",  "rm.wav.rtatmp"},  {"m.wv",  "rm.wv.rtatmp"},
        };
        for (const auto& c : pr) {
            const std::string a = dir + "/" + c.src;
            const std::string b = dir + "/" + c.dst;
            if (!exists(a) || !exists(b)) { skip(std::string("PCM 门禁 ") + c.src + "：样本缺失"); continue; }
            const std::string wa = dir + "/pcm_a.wav";
            const std::string wb = dir + "/pcm_b.wav";
            const std::wstring cmdA = L"\"" + rta::audio::widen(ffmpeg) + L"\" -v error -y -i \"" +
                                      rta::audio::widen(a) + L"\" -map 0:a:0 -vn -map_metadata -1 -f wav -c:a pcm_s32le \"" +
                                      rta::audio::widen(wa) + L"\"";
            const std::wstring cmdB = L"\"" + rta::audio::widen(ffmpeg) + L"\" -v error -y -i \"" +
                                      rta::audio::widen(b) + L"\" -map 0:a:0 -vn -map_metadata -1 -f wav -c:a pcm_s32le \"" +
                                      rta::audio::widen(wb) + L"\"";
            const int ra = run(cmdA), rb = run(cmdB);
            if (ra != 0 || rb != 0) { bad(std::string("PCM 门禁 ") + c.src + "：解码失败 ra=" + std::to_string(ra) + " rb=" + std::to_string(rb)); continue; }
            std::ifstream fa(fs::u8path(wa), std::ios::binary), fb(fs::u8path(wb), std::ios::binary);
            const std::vector<char> ba((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
            const std::vector<char> bb((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
            if (!ba.empty() && ba == bb)
                ok(std::string("PCM 门禁 ") + c.src + " 逐字节一致（" + std::to_string(ba.size()) + " 字节）");
            else
                bad(std::string("PCM 门禁 ") + c.src + " 不一致（" + std::to_string(ba.size()) + " vs " +
                    std::to_string(bb.size()) + " 字节）");
        }
    }

    // --- 3c. probe_media（media_info.h）：时长/码率等靠精简版 ffprobe ---
    {
        const std::string f = dir + "/m.flac";
        if (exists(f)) {
            const rta::audio::MediaInfo mi = rta::audio::probe_media(f);
            const bool okAll = mi.sampleRate == 48000 && mi.channels == 1 &&
                               mi.codec == "flac" && mi.bits == 24 &&
                               std::fabs((double)mi.duration - 0.6) < 0.1;
            char d[256];
            std::snprintf(d, sizeof d, "probe_media(flac): codec=%s rate=%d ch=%d bits=%d duration=%.3fs",
                          mi.codec.c_str(), mi.sampleRate, mi.channels, mi.bits, mi.duration);
            if (okAll) ok(d); else bad(d);
        } else skip("probe_media：样本缺失");
    }

    // --- 4. 内嵌封面（media_info.h 的 get_cover：ffmpeg 解码 mjpeg/png → 重新编码 PNG） ---
    {
        const std::string withCover = dir + "/m_cover.flac";
        if (exists(withCover)) {
            std::string mime;
            const auto bytes = rta::audio::get_cover(withCover, mime);
            const bool isPng = bytes.size() > 64 && bytes[0] == 0x89 && bytes[1] == 'P' &&
                               bytes[2] == 'N' && bytes[3] == 'G';
            if (isPng) ok("内嵌封面解码为 PNG（" + std::to_string(bytes.size()) + " 字节）");
            else       bad("内嵌封面解码失败（" + std::to_string(bytes.size()) + " 字节, mime=" + mime + "）");
        } else {
            skip("内嵌封面：样本未生成");
        }
    }

    // --- 5. ffprobe 位深探测（read_via_ffmpeg 用它决定 pcm_s16/24/32） ---
    {
        const std::string p = dir + "/m.flac";
        if (exists(p)) {
            const int bits = rta::audio::probe_bit_depth(ffprobe, p);
            if (bits == 24 || bits == 16 || bits == 32) ok("probe_bit_depth(flac) = " + std::to_string(bits));
            else bad("probe_bit_depth(flac) 返回异常值 " + std::to_string(bits));
        }
    }

    std::printf("[SUMMARY] pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

// ---------------- 父进程：准备环境 + 起子进程 ----------------
bool has_encoder(const std::string& ffmpeg, const std::string& name) {
    std::string out;
    if (run(L"\"" + rta::audio::widen(ffmpeg) + L"\" -hide_banner -encoders", &out) != 0) return false;
    return out.find(" " + name + " ") != std::string::npos;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc >= 3 && std::string(argv[1]) == "--child") {
        return child_main(std::string(argv[2]));
    }

    std::printf("=== 精简版 ffmpeg 音频格式矩阵 ===\n");
    const std::string self = exe_dir();
    // 精简版目录：环境变量优先，其次 <exe 目录>/ffmpeg-lite
    std::string liteDir;
    if (const char* env = std::getenv("RTA_FFMPEG_LITE_DIR")) liteDir = env;
    if (liteDir.empty()) liteDir = self + "\\ffmpeg-lite";
    if (!exists(liteDir + "\\ffmpeg.exe") || !exists(liteDir + "\\ffprobe.exe")) {
        skip("未找到精简版 ffmpeg/ffprobe（" + liteDir +
             "）。先跑 tools/build_ffmpeg_lite.ps1；本用例在只有系统 full 版时跳过（不判失败）");
        std::printf("[SUMMARY] pass=0 fail=0 (skipped)\n");
        return 0;
    }
    info("精简版目录 = " + liteDir);

    // 合成样本用的 ffmpeg：只查 PATH（需要编码器），精简版没有编码器
    const std::string gen = path_only("ffmpeg");
    if (gen.empty()) {
        bad("PATH 上没有 ffmpeg，无法合成测试样本（CI 里由 choco install ffmpeg 提供）");
        return 1;
    }
    info("样本合成器 = " + gen);

    wchar_t tmpRoot[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, tmpRoot) == 0) { bad("GetTempPathW 失败"); return 1; }
    const std::string dir = rta::audio::wide_to_utf8(tmpRoot) + "rta_ffmatrix_" +
                            std::to_string((long long)GetCurrentProcessId());
    std::error_code ec;
    fs::remove_all(fs::u8path(dir), ec);
    fs::create_directories(fs::u8path(dir), ec);
    info("临时目录 = " + dir);

    // 把精简版 + 测试自身复制进来：子进程的 find_tool 会命中同目录的精简版
    for (const char* n : {"ffmpeg.exe", "ffprobe.exe"})
        fs::copy_file(fs::u8path(liteDir + "\\" + n), fs::u8path(dir + "/" + n), fs::copy_options::overwrite_existing, ec);
    fs::copy_file(fs::u8path(self + "\\test_ffmpeg_matrix.exe"),
                  fs::u8path(dir + "/test_ffmpeg_matrix.exe"), fs::copy_options::overwrite_existing, ec);

    // 基础 PCM（−60 dBFS 1 kHz，0.6 s，24-bit）——绝不发声：只写文件
    const std::string base = dir + "/base.wav";
    const int rc0 = run(L"\"" + rta::audio::widen(gen) + L"\" -hide_banner -v error -y -f lavfi -i "
                        L"\"aevalsrc=0.001*sin(2*PI*1000*t):s=48000:d=0.6\" -c:a pcm_s24le \"" +
                        rta::audio::widen(base) + L"\"");
    if (rc0 != 0 || fsize(base) < 1000) {
        bad("合成基础 WAV 失败（rc=" + std::to_string(rc0) + "）");
        return 1;
    }
    info("基础 WAV = " + std::to_string(fsize(base)) + " 字节（−60 dBFS / 1 kHz / 48k / 24-bit）");

    struct Enc { const char* label; const char* out; const char* enc; const char* args; };
    const Enc encs[] = {
        {"flac",      "m.flac",     "flac",       ""},
        {"mp3",       "m.mp3",      "libmp3lame", "-b:a 192k"},
        {"aac(m4a)",  "m_aac.m4a",  "aac",        "-b:a 192k"},
        {"alac(m4a)", "m_alac.m4a", "alac",       ""},
        {"ogg-vorbis","m.ogg",      "libvorbis",  "-q:a 5"},
        {"opus",      "m.opus",     "libopus",    "-b:a 128k"},
        {"wav",       "m.wav",      "pcm_s24le",  ""},
        {"wma",       "m.wma",      "wmav2",      "-b:a 192k"},
        {"tta",       "m.tta",      "tta",        ""},
        {"wavpack",   "m.wv",       "wavpack",    ""},
    };
    std::vector<std::string> uncovered;
    for (const auto& e : encs) {
        if (!has_encoder(gen, e.enc)) { uncovered.push_back(std::string(e.label) + "（无 " + e.enc + " 编码器）"); continue; }
        const std::wstring cmd = L"\"" + rta::audio::widen(gen) + L"\" -hide_banner -v error -y -i \"" +
                                 rta::audio::widen(base) + L"\" -c:a " + rta::audio::widen(e.enc) + L" " +
                                 rta::audio::widen(e.args) + L" \"" + rta::audio::widen(dir + "/" + e.out) + L"\"";
        std::string log;
        const int rc = run(cmd, &log);
        if (rc != 0 || fsize(dir + "/" + e.out) < 200) {
            std::string s = log.substr(0, 200);
            for (char& c : s) if (c == '\n' || c == '\r') c = ' ';
            uncovered.push_back(std::string(e.label) + "（编码失败 rc=" + std::to_string(rc) + "：" + s + "）");
        }
    }

    // DSD：ffmpeg 9.0 没有 DSD 编码器 → 自己合成（见文件头 ⚠️）
    if (!write_dsf(dir + "/m.dsf", 2822400, 2, 0.5, 0.001)) uncovered.push_back("dsf（本地合成失败）");
    if (!write_dff(dir + "/m.dff", 2822400, 2, 0.5, 0.001)) uncovered.push_back("dff（本地合成失败）");

    // 带内嵌封面的 FLAC：先造一张 PNG，再用 -map 把图作为 attached_pic 塞进 FLAC
    {
        const std::string png = dir + "/cover_src.png";
        std::string log;
        const std::wstring cmd = L"\"" + rta::audio::widen(gen) + L"\" -hide_banner -v error -y -f lavfi -i "
                                 L"\"color=c=red:s=64x64:d=1\" -frames:v 1 \"" + rta::audio::widen(png) + L"\"";
        if (run(cmd, &log) == 0 && exists(png)) {
            const std::wstring cmd2 = L"\"" + rta::audio::widen(gen) + L"\" -hide_banner -v error -y -i \"" +
                                      rta::audio::widen(dir + "/m.flac") + L"\" -i \"" +
                                      rta::audio::widen(png) + L"\" -map 0:a -map 1:v -c copy "
                                      L"-disposition:v attached_pic \"" + rta::audio::widen(dir + "/m_cover.flac") + L"\"";
            if (run(cmd2, &log) != 0 || fsize(dir + "/m_cover.flac") < 500)
                skip("内嵌封面样本生成失败（不影响解码矩阵）");
        } else {
            skip("封面源图生成失败（不影响解码矩阵）");
        }
    }

    if (!uncovered.empty()) {
        for (const auto& u : uncovered) std::printf("[UNCOVERED] %s —— 现场合成不了，本测试不对其做解码断言\n", u.c_str());
    }
    std::printf("[UNCOVERED] ape / mpc7 / mpc8 / tak / shorten / dst —— ffmpeg 只有解码器没有编码器，"
                "无法现场合成；它们的覆盖由「解码器白名单」断言（见 tools/ffmpeg_lite_required.txt）\n");

    // 起子进程（在临时目录里，保证 find_tool 命中精简版）
    const std::wstring childCmd = L"\"" + rta::audio::widen(dir + "/test_ffmpeg_matrix.exe") +
                                  L"\" --child \"" + rta::audio::widen(dir) + L"\"";
    std::string childOut;
    const int rc = rta::audio::run_hidden(childCmd, &childOut);
    std::printf("%s", childOut.c_str());
    if (childOut.find("[SUMMARY]") == std::string::npos) {
        std::printf("[FAIL] 子进程没有输出 [SUMMARY]（rc=%d）——很可能没跑起来\n", rc);
        g_fail++;
    }
    if (rc != 0) { std::printf("[FAIL] 子进程退出码 %d\n", rc); g_fail++; }

    std::error_code ec2;
    fs::remove_all(fs::u8path(dir), ec2);
    std::printf("[SUMMARY] parent_fail=%d (子进程内已分别统计)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}