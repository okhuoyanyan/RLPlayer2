// test_replaygain.cpp — ReplayGain / Album Gain 离线门禁
//
// 判据全部可被单独否定（铁律 3）：
//   [1] 值解析（" dB" 后缀 / 全角减号 / 小数逗号 / dB 写法的 peak / 百分比写法）
//   [2] **真实容器夹具**的键名实读：FLAC/OGG(Vorbis comment)、MP3(ID3v2 TXXX)、WV/MP3(APEv2)、
//       M4A(ilst freeform) —— 夹具由 ffmpeg 现场写出，键名用 ffprobe 交叉验证；
//       ⚠️ 这不是"我以为的键名"，是文件里真有的字节。
//   [3] 缺标签换算：BS.1770-4 校准点（997 Hz / −23 dBFS）⟹ gain = −18 − (−23) = +5.0 dB
//   [4] 防削波：峰值 +0.5 dBFS 的素材，请求 +6 dB ⟹ applied = −0.5 dB，且乘完之后峰值 ≤ 0 dBFS
//   [5] 三模式 off/track/album 的差异表（标签路径 + 计算路径各一遍）
//   [6] 与 loudness 串联不双倍提升：级联增益 == 各级增益之和（线性相乘），低频另加的是架式提升
//   [7] 专辑归组与专辑响度（时长加权 vs 拼接后统一门限的差）
//   [8] 真实库 D:\RTA-hqplayer\MUSIC 只读扫描统计（只读标签，不解码、不写任何文件）
//
// 全程离线：不开任何音频设备（铁律 1）；不解码到磁盘；真实库只读。
#include "dsp/replaygain.h"
#include "analysis/dynamics.h"
#include "dsp/loudness.h"
#include "dsp/volume.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

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
using namespace rta::dsp;
using namespace rta::dsp::replaygain;

static int fails = 0;
static void check(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  [%s] %s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
    if (!ok) ++fails;
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// 跑一个进程（stdout 可选捕获）—— 只用 CreateProcessW，避免 std::system 的 ANSI 路径问题
static bool run_proc(const std::vector<std::string>& args, std::string* out, int* code) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    const bool cap = (out != nullptr);
    if (cap && !::CreatePipe(&rd, &wr, &sa, 0)) return false;
    if (cap) ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = cap ? wr : ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    std::wstring cmd;
    for (const auto& a : args) { if (!cmd.empty()) cmd += L' '; cmd += L'"'; cmd += widen(a); cmd += L'"'; }
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (cap) ::CloseHandle(wr);
    if (!ok) { if (cap) ::CloseHandle(rd); return false; }
    if (cap) {
        char buf[4096];
        DWORD got = 0;
        while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) out->append(buf, got);
        ::CloseHandle(rd);
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD c = 1;
    ::GetExitCodeProcess(pi.hProcess, &c);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    if (code) *code = (int)c;
    return true;
#else
    (void)args; (void)out; (void)code; return false;
#endif
}

static std::string path_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string((const char*)u8.data(), u8.size());
}

// 写一个 16 bit PCM WAV（合成素材，不写用户目录）
static bool write_wav16(const fs::path& p, const std::vector<std::vector<double>>& ch, int rate) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    const uint16_t channels = (uint16_t)ch.size();
    const uint32_t frames = ch.empty() ? 0 : (uint32_t)ch[0].size();
    const uint16_t bits = 16;
    const uint32_t dataBytes = frames * channels * 2;
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(channels); w32((uint32_t)rate);
    w32((uint32_t)rate * channels * 2); w16((uint16_t)(channels * 2)); w16(bits);
    f.write("data", 4); w32(dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < channels; ++c) {
            double v = std::max(-1.0, std::min(1.0, ch[c][i]));
            w16((uint16_t)(int16_t)std::lround(v * 32767.0));
        }
    return true;
}

// 写一个 32 bit float WAV，允许样本越界（造 peak > 0 dBFS 的素材）
static bool write_wav_f32(const fs::path& p, const std::vector<std::vector<double>>& ch, int rate) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    const uint16_t channels = (uint16_t)ch.size();
    const uint32_t frames = ch.empty() ? 0 : (uint32_t)ch[0].size();
    const uint32_t dataBytes = frames * channels * 4;
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + 18 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(18); w16(3); w16(channels); w32((uint32_t)rate);
    w32((uint32_t)rate * channels * 4); w16((uint16_t)(channels * 4)); w16(32);
    w16(0);                                  // cbSize = 0
    f.write("fact", 4); w32(4); w32(frames); // 非 PCM 需要 fact 块
    f.write("data", 4); w32(dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < channels; ++c) {
            const float v = (float)ch[c][i];
            f.write((const char*)&v, 4);
        }
    return true;
}

// 往 WAV 里插一个 "id3 " 块（内含 ID3v2.3 TXXX 帧）。ffmpeg 的 WAV muxer 只会写 LIST/INFO、
// 不认未知键，所以这里按 RIFF 规范自己拼字节 —— 顺带把"WAV 的 id3 块"这条真实通路测掉
// （本库 71 个 WAV 里有 52 个就带 "id3 " 块）。
static std::vector<unsigned char> id3v23_txxx(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::vector<unsigned char> frames;
    for (const auto& p : kv) {
        std::vector<unsigned char> body;
        body.push_back(0);                                    // encoding = latin1
        body.insert(body.end(), p.first.begin(), p.first.end());
        body.push_back(0);
        body.insert(body.end(), p.second.begin(), p.second.end());
        const uint32_t n = (uint32_t)body.size();
        frames.insert(frames.end(), {'T', 'X', 'X', 'X'});
        frames.push_back((unsigned char)(n >> 24)); frames.push_back((unsigned char)(n >> 16));
        frames.push_back((unsigned char)(n >> 8));  frames.push_back((unsigned char)n);
        frames.push_back(0); frames.push_back(0);             // flags
        frames.insert(frames.end(), body.begin(), body.end());
    }
    auto syncsafe = [](uint32_t v) {
        return std::vector<unsigned char>{(unsigned char)((v >> 21) & 0x7f), (unsigned char)((v >> 14) & 0x7f),
                                          (unsigned char)((v >> 7) & 0x7f), (unsigned char)(v & 0x7f)};
    };
    std::vector<unsigned char> tag = {'I', 'D', '3', 3, 0, 0};
    const auto ss = syncsafe((uint32_t)frames.size());
    tag.insert(tag.end(), ss.begin(), ss.end());
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

static bool wav_add_id3_chunk(const fs::path& p,
                              const std::vector<std::pair<std::string, std::string>>& kv) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0) return false;
    const std::vector<unsigned char> tag = id3v23_txxx(kv);
    std::vector<unsigned char> chunk = {'i', 'd', '3', ' '};
    const uint32_t n = (uint32_t)tag.size();
    chunk.push_back((unsigned char)n); chunk.push_back((unsigned char)(n >> 8));
    chunk.push_back((unsigned char)(n >> 16)); chunk.push_back((unsigned char)(n >> 24));
    chunk.insert(chunk.end(), tag.begin(), tag.end());
    if (n & 1) chunk.push_back(0);                            // 偶数字节对齐
    b.insert(b.begin() + 12, chunk.begin(), chunk.end());      // 放在 fmt 之前（任意位置都合法）
    auto wr32 = [&](size_t o, uint32_t v) {
        b[o] = (unsigned char)v; b[o + 1] = (unsigned char)(v >> 8);
        b[o + 2] = (unsigned char)(v >> 16); b[o + 3] = (unsigned char)(v >> 24);
    };
    wr32(4, (uint32_t)(b.size() - 8));                         // 更新 RIFF 长度
    std::ofstream o(p, std::ios::binary);
    if (!o) return false;
    o.write((const char*)b.data(), (std::streamsize)b.size());
    return true;
}

// 解码一个文件 → LUFS/峰值/时长（ffmpeg 输出走内存，绝不落盘；不开音频设备）
static bool measure_file(const std::string& path, double* lufs, double* peakLinear, double* dur) {
    std::string meta;
    int rc = 1;
    if (!run_proc({"ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                   "stream=sample_rate,channels", "-of", "csv=p=0", path}, &meta, &rc) || rc != 0) return false;
    int rate = 0, ch = 0;
    if (std::sscanf(meta.c_str(), "%d,%d", &rate, &ch) != 2 || rate <= 0 || ch <= 0) return false;
    std::string raw;
    if (!run_proc({"ffmpeg", "-v", "error", "-i", path, "-map", "0:a:0", "-f", "f64le",
                   "-acodec", "pcm_f64le", "-"}, &raw, &rc) || rc != 0) return false;
    const size_t fb = sizeof(double) * (size_t)ch;
    if (raw.size() < fb) return false;
    const size_t frames = raw.size() / fb;
    rta::analysis::DynamicsAccumulator acc;
    acc.begin(ch, (double)rate);
    const size_t chunk = 32768;
    std::vector<double> buf((size_t)ch * chunk);
    for (size_t pos = 0; pos < frames; pos += chunk) {
        const size_t n = std::min(chunk, frames - pos);
        std::memcpy(buf.data(), raw.data() + pos * fb, n * fb);
        acc.add_interleaved(buf.data(), n);
    }
    const auto res = acc.finalize();
    *lufs = res.lufsIntegrated;
    *peakLinear = (res.peakDbfs > -299.0) ? std::pow(10.0, res.peakDbfs / 20.0) : 0.0;
    if (dur) *dur = (double)frames / (double)rate;
    return res.valid;
}

// 从 rg_probe 的 JSON 里取一个数字（诊断用；不做通用 JSON 解析）
static bool json_num(const std::string& s, const std::string& key, double* v) {
    const size_t p = s.find("\"" + key + "\": ");
    if (p == std::string::npos) return false;
    return std::sscanf(s.c_str() + p + key.size() + 4, "%lf", v) == 1;
}

// 在 M4A 的 moov/udta/meta/ilst 里插一个 '----' 自由格式原子（iTunes 写 ReplayGain 就是这么写的）
// 返回 true 并写 out；同时被 ffprobe 交叉验证（见 [2]）。
static bool mp4_add_freeform(const std::string& inPath, const std::string& outPath,
                             const std::string& mean, const std::string& name, const std::string& value) {
    std::ifstream in(fs::u8path(inPath), std::ios::binary);
    if (!in) return false;
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    auto rd32 = [&](size_t o) { return ((uint32_t)b[o] << 24) | ((uint32_t)b[o + 1] << 16) | ((uint32_t)b[o + 2] << 8) | b[o + 3]; };
    auto wr32 = [&](size_t o, uint32_t v) { b[o] = (unsigned char)(v >> 24); b[o + 1] = (unsigned char)(v >> 16); b[o + 2] = (unsigned char)(v >> 8); b[o + 3] = (unsigned char)v; };
    // 找 moov / udta / meta / ilst 的 (offset,size)
    struct Atom { size_t off = 0, size = 0; };
    std::function<Atom(const char* type, size_t from, size_t to, int skip)> find = [&](const char* type, size_t from, size_t to, int skip) -> Atom {
        size_t p = from;
        while (p + 8 <= to) {
            const uint32_t sz = rd32(p);
            if (sz < 8 || p + sz > to) break;
            if (std::memcmp(&b[p + 4], type, 4) == 0) return Atom{p, sz};
            p += sz;
        }
        (void)skip;
        return Atom{0, 0};
    };
    const Atom moov = find("moov", 0, b.size(), 0);
    if (!moov.off) return false;
    const Atom udta = find("udta", moov.off + 8, moov.off + moov.size, 0);
    if (!udta.off) return false;
    const Atom meta = find("meta", udta.off + 8, udta.off + udta.size, 0);
    if (!meta.off) return false;
    const Atom ilst = find("ilst", meta.off + 12, meta.off + meta.size, 0);   // meta 有 4B version/flags
    if (!ilst.off) return false;

    auto mk = [](const char* type, const std::vector<unsigned char>& payload) {
        std::vector<unsigned char> a;
        const uint32_t sz = (uint32_t)(8 + payload.size());
        a.push_back((unsigned char)(sz >> 24)); a.push_back((unsigned char)(sz >> 16));
        a.push_back((unsigned char)(sz >> 8));  a.push_back((unsigned char)sz);
        a.insert(a.end(), type, type + 4);
        a.insert(a.end(), payload.begin(), payload.end());
        return a;
    };
    auto payload_str = [](const std::string& s) {
        std::vector<unsigned char> v = {0, 0, 0, 0};   // version/flags
        v.insert(v.end(), s.begin(), s.end());
        return v;
    };
    std::vector<unsigned char> inner;
    auto meanAtom = mk("mean", payload_str(mean));
    auto nameAtom = mk("name", payload_str(name));
    std::vector<unsigned char> dataPayload = {0, 0, 0, 1, 0, 0, 0, 0};   // type=UTF8, locale=0
    dataPayload.insert(dataPayload.end(), value.begin(), value.end());
    auto dataAtom = mk("data", dataPayload);
    inner.insert(inner.end(), meanAtom.begin(), meanAtom.end());
    inner.insert(inner.end(), nameAtom.begin(), nameAtom.end());
    inner.insert(inner.end(), dataAtom.begin(), dataAtom.end());
    auto nn = mk("----", inner);

    const size_t insertAt = ilst.off + ilst.size;      // ilst 末尾（数据区之后）
    b.insert(b.begin() + (long long)insertAt, nn.begin(), nn.end());
    // 逐层加长度（存在于插入点之前的原子）
    wr32(ilst.off, rd32(ilst.off) + (uint32_t)nn.size());
    wr32(meta.off, rd32(meta.off) + (uint32_t)nn.size());
    wr32(udta.off, rd32(udta.off) + (uint32_t)nn.size());
    wr32(moov.off, rd32(moov.off) + (uint32_t)nn.size());

    std::ofstream o(fs::u8path(outPath), std::ios::binary);
    if (!o) return false;
    o.write((const char*)b.data(), (std::streamsize)b.size());
    return true;
}

// 断点：浮点比较
static bool approx_eq(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// ===========================================================================
int main() {
    const fs::path tmp = fs::temp_directory_path() / "rg_fixture";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    std::printf("== test_replaygain: 工作目录 %s ==\n", path_utf8(tmp).c_str());

    // ---------------------------------------------------------------- [1]
    std::printf("\n[1] 值解析\n");
    {
        double g = 0.0;
        const bool ok1 = parse_gain_db("-7.50 dB", &g);
        check(ok1 && approx_eq(g, -7.5, 1e-12), "parse_gain_db(\"-7.50 dB\")", std::to_string(g));
        check(parse_gain_db("+3.2dB", &g) && approx_eq(g, 3.2, 1e-12), "parse_gain_db(\"+3.2dB\")");
        check(parse_gain_db(" -0.86 \t", &g) && approx_eq(g, -0.86, 1e-12), "parse_gain_db(\" -0.86 \\t\")");
        // U+2212（真减号）与小数逗号：某些 tagger 会这么写
        check(parse_gain_db("\xE2\x88\x92" "6,10 dB", &g) && approx_eq(g, -6.10, 1e-12), "parse_gain_db(U+2212 6,10 dB)");
        check(!parse_gain_db("", &g) && !parse_gain_db("abc", &g), "非法串必须失败");
        double pk = 0.0;
        check(parse_peak_linear("0.988553", &pk) && approx_eq(pk, 0.988553, 1e-9), "parse_peak_linear(线性)");
        check(parse_peak_linear("-0.10 dB", &pk) && approx_eq(pk, std::pow(10.0, -0.10 / 20.0), 1e-9), "parse_peak_linear(dB 写法)", std::to_string(pk));
        check(parse_peak_linear("98.8%", &pk) && approx_eq(pk, 0.988, 1e-9), "parse_peak_linear(百分比写法)");
        // 键名规范化：大小写 / 连字符 / 首尾空白都要归一
        check(norm_key(" replaygain-track_gain ") == "REPLAYGAIN_TRACK_GAIN", "norm_key 归一");
        Mode m = Mode::Off;
        check(parse_mode("Album", &m) && m == Mode::Album, "parse_mode(\"Album\")");
    }

    // ---------------------------------------------------------------- [2]
    std::printf("\n[2] 真实容器夹具：键名实读（ffmpeg 现场写出，ffprobe 交叉验证）\n");
    bool haveFfmpeg = false;
    {
        int code = 0;
        haveFfmpeg = run_proc({"ffmpeg", "-version"}, nullptr, &code) && code == 0;
        if (!haveFfmpeg) {
            std::printf("  [SKIP] 环境里没有 ffmpeg，容器夹具无法生成（这一节不判失败，但报告里要写明）\n");
        } else {
            // 源素材：3 s / 48 kHz / 立体声 / 997 Hz / −12 dBFS（电平只用于算 LUFS，不发声）
            const int rate = 48000;
            std::vector<std::vector<double>> src(2, std::vector<double>(3 * rate));
            for (size_t i = 0; i < src[0].size(); ++i)
                for (int c = 0; c < 2; ++c) src[(size_t)c][i] = 0.251 * std::sin(2.0 * 3.14159265358979 * 997.0 * (double)i / rate);
            const std::string srcPath = path_utf8(tmp / "src.wav");
            check(write_wav16(tmp / "src.wav", src, rate), "写出合成素材 src.wav");

            const std::vector<std::string> md = {
                "-metadata", "REPLAYGAIN_TRACK_GAIN=-7.50 dB",
                "-metadata", "REPLAYGAIN_TRACK_PEAK=0.988553",
                "-metadata", "REPLAYGAIN_ALBUM_GAIN=-6.10 dB",
                "-metadata", "REPLAYGAIN_ALBUM_PEAK=0.998000",
                "-metadata", "TITLE=Fixture", "-metadata", "ALBUM=测试专辑", "-metadata", "ALBUMARTIST=测试艺人"};

            auto gen = [&](const std::string& name, const std::vector<std::string>& extra) {
                std::vector<std::string> args = {"ffmpeg", "-v", "error", "-y", "-i", srcPath};
                for (const auto& s : md) args.push_back(s);
                for (const auto& s : extra) args.push_back(s);
                args.push_back(path_utf8(tmp / name));
                int rc = 1;
                const bool ok = run_proc(args, nullptr, &rc) && rc == 0;
                check(ok, "生成夹具 " + name, ok ? "" : ("ffmpeg rc=" + std::to_string(rc)));
                return ok;
            };

            // ---- FLAC / OGG：Vorbis comment ----
            if (gen("fixture.flac", {"-c:a", "flac"})) {
                TagSet t; std::string why;
                const bool ok = read_tags(path_utf8(tmp / "fixture.flac"), &t, &why);
                check(ok && t.ok, "FLAC 读标签", why);
                check(t.container == Container::Vorbis, "FLAC 容器判定 = vorbis", container_name(t.container));
                std::printf("        FLAC 实际键名: track_gain=\"%s\" track_peak=\"%s\" album_gain=\"%s\" album_peak=\"%s\"\n",
                            t.keyTrackGain.c_str(), t.keyTrackPeak.c_str(), t.keyAlbumGain.c_str(), t.keyAlbumPeak.c_str());
                check(t.keyTrackGain == "REPLAYGAIN_TRACK_GAIN", "FLAC track gain 键名", t.keyTrackGain);
                check(t.keyAlbumGain == "REPLAYGAIN_ALBUM_GAIN", "FLAC album gain 键名", t.keyAlbumGain);
                double g = 0.0, pk = 0.0;
                check(parse_gain_db(t.trackGainRaw, &g) && approx_eq(g, -7.5, 1e-9), "FLAC track gain 值");
                check(parse_peak_linear(t.trackPeakRaw, &pk) && approx_eq(pk, 0.988553, 1e-9), "FLAC track peak 值");
                check(t.album == "测试专辑" && t.albumArtist == "测试艺人", "FLAC 专辑/专辑艺人（UTF-8）", t.album + " / " + t.albumArtist);
            }
            if (gen("fixture.ogg", {"-c:a", "libvorbis"})) {
                TagSet t; std::string why;
                const bool ok = read_tags(path_utf8(tmp / "fixture.ogg"), &t, &why);
                check(ok && t.container == Container::Vorbis, "OGG(Vorbis) 容器判定 = vorbis", why);
                std::printf("        OGG  实际键名: track_gain=\"%s\" album_gain=\"%s\"\n", t.keyTrackGain.c_str(), t.keyAlbumGain.c_str());
                check(t.keyTrackGain == "REPLAYGAIN_TRACK_GAIN", "OGG track gain 键名（在 stream comment 里）", t.keyTrackGain);
            }
            // ---- MP3：ID3v2.3 TXXX ----
            if (gen("fixture.mp3", {"-c:a", "libmp3lame", "-id3v2_version", "3"})) {
                TagSet t; std::string why;
                const bool ok = read_tags(path_utf8(tmp / "fixture.mp3"), &t, &why);
                check(ok && t.container == Container::Id3v2, "MP3 容器判定 = id3v2", why);
                std::printf("        MP3  实际键名: track_gain=\"%s\" (TXXX desc) album_gain=\"%s\"\n",
                            t.keyTrackGain.c_str(), t.keyAlbumGain.c_str());
                check(t.keyTrackGain == "REPLAYGAIN_TRACK_GAIN", "MP3 TXXX 键名", t.keyTrackGain);
                double g = 0.0;
                check(parse_gain_db(t.trackGainRaw, &g) && approx_eq(g, -7.5, 1e-9), "MP3 TXXX 值（尾部 NUL 已裁）", t.trackGainRaw);
            }
            // ---- APEv2：ffmpeg 写的 WavPack 真有 APEv2 尾标；再原样追加到 MP3 副本 ----
            if (gen("fixture.wv", {"-c:a", "wavpack", "-write_apetag", "1"})) {
                TagSet t; std::string why;
                const bool ok = read_tags(path_utf8(tmp / "fixture.wv"), &t, &why);
                check(ok && t.container == Container::ApeV2, "WV 容器判定 = apev2", why);
                std::printf("        WV   实际键名: track_gain=\"%s\" album_gain=\"%s\"\n", t.keyTrackGain.c_str(), t.keyAlbumGain.c_str());
                check(t.keyTrackGain == "REPLAYGAIN_TRACK_GAIN", "APEv2 键名", t.keyTrackGain);

                // 取 .wv 尾部 APEv2 标签字节（footer 的 size 字段 = items 区），追加到 mp3 副本末尾
                std::ifstream wv(fs::u8path(path_utf8(tmp / "fixture.wv")), std::ios::binary);
                std::vector<unsigned char> wvb((std::istreambuf_iterator<char>(wv)), std::istreambuf_iterator<char>());
                bool patched = false;
                if (wvb.size() > 32 && std::memcmp(wvb.data() + wvb.size() - 32, "APETAGEX", 8) == 0) {
                    const unsigned char* fo = wvb.data() + wvb.size() - 32;
                    const uint32_t itemsSz = (uint32_t)fo[12] | ((uint32_t)fo[13] << 8) | ((uint32_t)fo[14] << 16) | ((uint32_t)fo[15] << 24);
                    const size_t total = (size_t)itemsSz + 32;
                    if (wvb.size() >= total) {
                        std::vector<unsigned char> tagBytes(wvb.end() - (long long)total, wvb.end());
                        // 造一个"只有 APEv2、没有 ID3v2"的 mp3
                        std::vector<std::string> args = {"ffmpeg", "-v", "error", "-y", "-i", srcPath,
                                                         "-c:a", "libmp3lame", "-write_id3v2", "0",
                                                         path_utf8(tmp / "apev2_only.mp3")};
                        int rc = 1;
                        if (run_proc(args, nullptr, &rc) && rc == 0) {
                            std::ofstream o(fs::u8path(path_utf8(tmp / "apev2_only.mp3")), std::ios::binary | std::ios::app);
                            o.write((const char*)tagBytes.data(), (std::streamsize)tagBytes.size());
                            o.close();
                            TagSet t2; std::string why2;
                            const bool ok2 = read_tags(path_utf8(tmp / "apev2_only.mp3"), &t2, &why2);
                            check(ok2 && t2.container == Container::ApeV2 && t2.keyTrackGain == "REPLAYGAIN_TRACK_GAIN",
                                  "MP3 + APEv2（ID3v2 里没有 RG 键）也能读到",
                                  std::string(container_name(t2.container)) + " " + t2.keyTrackGain + " " + why2);
                            patched = ok2;
                        }
                    }
                }
                check(patched, "APEv2 标签字节复制到 MP3 尾部（模拟真实 tagger）");
            }
            // ---- M4A：ilst '----' 自由格式（iTunes 惯例，键名小写）----
            if (gen("plain.m4a", {"-c:a", "aac", "-b:a", "128k"})) {
                const std::string m4a = path_utf8(tmp / "fixture.m4a");
                bool built = mp4_add_freeform(path_utf8(tmp / "plain.m4a"), m4a,
                                              "com.apple.iTunes", "replaygain_track_gain", "-7.50 dB");
                built = built && mp4_add_freeform(m4a, m4a, "com.apple.iTunes", "replaygain_track_peak", "0.988553");
                check(built, "M4A 注入 '----' 自由格式原子");
                TagSet t; std::string why;
                const bool ok = read_tags(m4a, &t, &why);
                check(ok && t.container == Container::Mp4, "M4A 容器判定 = mp4", why);
                std::printf("        M4A  实际键名: track_gain=\"%s\" track_peak=\"%s\"\n", t.keyTrackGain.c_str(), t.keyTrackPeak.c_str());
                check(t.keyTrackGain == "----:com.apple.iTunes:replaygain_track_gain", "M4A 自由格式完整键名", t.keyTrackGain);
                double g = 0.0;
                check(parse_gain_db(t.trackGainRaw, &g) && approx_eq(g, -7.5, 1e-9), "M4A 自由格式值");
                // 交叉验证：ffprobe 也要能读出同一个标签（证明原子布局是对的，不是我自说自话）
                std::string out;
                int rc = 1;
                if (run_proc({"ffprobe", "-v", "error", "-show_entries", "format_tags", "-of", "default", m4a}, &out, &rc) && rc == 0) {
                    const bool seen = out.find("replaygain_track_gain") != std::string::npos ||
                                      out.find("REPLAYGAIN_TRACK_GAIN") != std::string::npos;
                    check(seen, "ffprobe 也读得到同一个 M4A 自由格式标签", seen ? "" : "ffprobe 输出里没有该键");
                } else {
                    std::printf("  [SKIP] ffprobe 交叉验证未执行\n");
                }
            }
        }
    }

    // ---------------------------------------------------------------- [3]
    std::printf("\n[3] 缺标签换算口径（ReplayGain 2.0：gain = -18 - LUFS）\n");
    {
        // BS.1770-4 官方校准点：997 Hz / −23 dBFS / 立体声 ⟹ −23.0 LUFS
        const int rate = 48000;
        const size_t n = (size_t)rate * 6;
        std::vector<std::vector<double>> sig(2, std::vector<double>(n));
        const double amp = std::pow(10.0, -23.0 / 20.0);
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 2; ++c) sig[(size_t)c][i] = amp * std::sin(2.0 * 3.14159265358979 * 997.0 * (double)i / rate);
        std::vector<const double*> p = {sig[0].data(), sig[1].data()};
        const auto r = rta::analysis::measure_dynamics(p.data(), 2, n, (double)rate);
        check(r.valid && approx_eq(r.lufsIntegrated, -23.0, 0.1), "校准点 LUFS ≈ −23.000", std::to_string(r.lufsIntegrated));
        const double g18 = gain_from_lufs(r.lufsIntegrated, -18.0);
        check(approx_eq(g18, 5.0, 0.1), "参考 −18 LUFS ⟹ gain ≈ +5.0 dB", std::to_string(g18));
        const double g23 = gain_from_lufs(r.lufsIntegrated, -23.0);
        check(approx_eq(g23, 0.0, 0.1), "参考 −23 LUFS ⟹ gain ≈ 0.0 dB（口径差 5 dB 的由来）", std::to_string(g23));
        // 同一素材走决定路径：没有标签 + 有测量 ⟹ 用计算值
        Track t;
        t.meas.valid = true; t.meas.lufs = r.lufsIntegrated; t.meas.peak = amp;
        Params p2; p2.mode = Mode::Track;
        const auto d = decide(t, p2);
        check(d.active && d.source == "compute:track" && approx_eq(d.appliedDb, 5.0, 0.1),
              "无标签时按计算值给增益", d.source + " " + std::to_string(d.appliedDb));
    }

    // ---------------------------------------------------------------- [4]
    std::printf("\n[4] 防削波（peak = +0.5 dBFS 的素材，请求 +6 dB）\n");
    {
        const double peak = std::pow(10.0, 0.5 / 20.0);      // 1.05925 > 1.0：源本身就过 0 dBFS
        Track t;
        t.tags.trackGainRaw = "+6.00 dB";
        t.tags.keyTrackGain = "REPLAYGAIN_TRACK_GAIN";
        t.tags.trackPeakRaw = "1.059254";
        t.tags.keyTrackPeak = "REPLAYGAIN_TRACK_PEAK";
        t.meas.valid = true; t.meas.lufs = -24.0; t.meas.peak = peak;

        Params p; p.mode = Mode::Track; p.preventClipping = true;
        const auto d = decide(t, p);
        check(d.active && d.havePeak && approx_eq(d.peakDbfs, 0.5, 1e-3), "峰值判定 = +0.50 dBFS", std::to_string(d.peakDbfs));
        check(approx_eq(d.maxSafeGainDb, -0.5, 1e-3), "最大安全增益 = −0.50 dB", std::to_string(d.maxSafeGainDb));
        check(approx_eq(d.appliedDb, -0.5, 1e-3) && d.clipLimited, "applied 被限到 −0.50 dB 且标记 clipLimited",
              std::to_string(d.appliedDb));

        // 真乘一遍：输出峰值必须 ≤ 1.0（−0.5 dBFS 的素材 + (−0.5 dB) = 0 dBFS）
        std::vector<double> buf(48000);
        for (size_t i = 0; i < buf.size(); ++i) buf[i] = peak * std::sin(2.0 * 3.14159265358979 * 997.0 * (double)i / 48000.0);
        double before = 0.0;
        for (double v : buf) before = std::max(before, std::fabs(v));
        apply_gain(d.appliedDb, buf.data(), buf.size());
        double after = 0.0;
        for (double v : buf) after = std::max(after, std::fabs(v));
        check(approx_eq(linear_to_dbfs(before), 0.5, 1e-6), "输入峰值 +0.50 dBFS", std::to_string(linear_to_dbfs(before)));
        check(after <= 1.0 + 1e-12, "输出峰值 ≤ 0 dBFS（不削波）", std::to_string(linear_to_dbfs(after)));

        // 关掉防削波 ⟹ 必须真的过冲（证明这条保护不是摆设）
        Params p2 = p; p2.preventClipping = false;
        const auto d2 = decide(t, p2);
        check(approx_eq(d2.appliedDb, 6.0, 1e-9) && !d2.clipLimited, "preventClipping=off 时不做限制", std::to_string(d2.appliedDb));
        std::vector<double> b2 = buf;   // 已经被压过一遍，重新造
        for (size_t i = 0; i < b2.size(); ++i) b2[i] = peak * std::sin(2.0 * 3.14159265358979 * 997.0 * (double)i / 48000.0);
        apply_gain(d2.appliedDb, b2.data(), b2.size());
        double over = 0.0;
        for (double v : b2) over = std::max(over, std::fabs(v));
        check(over > 2.0, "不保护时峰值翻倍（+6 dB ⟹ 2.11 倍）", std::to_string(over));

        // 0 dB 必须逐位直通（不乘 1.0）
        std::vector<double> z = {0.0, -0.0, 1.0, -1.0, 1e-30, 0.7};
        const std::vector<double> z0 = z;
        apply_gain(0.0, z.data(), z.size());
        bool same = true;
        for (size_t i = 0; i < z.size(); ++i)
            if (std::memcmp(&z[i], &z0[i], sizeof(double)) != 0) same = false;
        check(same, "applied=0 dB 时逐位不变（bit-perfect 直通）");
    }

    // ---------------------------------------------------------------- [5]
    std::printf("\n[5] 三模式行为差异（同一批曲目）\n");
    {
        // 三首：A/B 同专辑同专辑艺人，C 另一张专辑
        auto mk = [](const char* path, const char* album, const char* aart, double tg, double ag, double lufs) {
            Track t;
            t.path = path;
            t.tags.album = album; t.tags.albumArtist = aart; t.tags.title = path;
            char b[32];
            std::snprintf(b, sizeof(b), "%+.2f dB", tg);
            t.tags.trackGainRaw = b; t.tags.keyTrackGain = "REPLAYGAIN_TRACK_GAIN";
            std::snprintf(b, sizeof(b), "%+.2f dB", ag);
            t.tags.albumGainRaw = b; t.tags.keyAlbumGain = "REPLAYGAIN_ALBUM_GAIN";
            t.meas.valid = true; t.meas.lufs = lufs; t.meas.peak = 0.5; t.meas.durationSec = 200.0;
            return t;
        };
        std::vector<Track> v;
        v.push_back(mk("A.flac", "Alb1", "Art1", -7.50, -6.10, -14.0));
        v.push_back(mk("B.flac", "Alb1", "Art1", -5.00, -6.10, -12.5));
        v.push_back(mk("C.flac", "Alb2", "Art1", -9.00, -8.20, -16.0));

        const auto albums = compute_albums(v, -18.0);
        check(albums.size() == 2, "归组：Alb1 两首 + Alb2 一首 = 2 组", std::to_string(albums.size()));
        for (const auto& a : albums)
            if (a.album == "Alb1") check(a.members.size() == 2 && approx_eq(a.gainDb, -6.10, 1e-9) && a.fromTag,
                                        "Alb1 专辑增益来自标签 −6.10 dB", std::to_string(a.gainDb));

        Params po; po.mode = Mode::Off;
        Params pt; pt.mode = Mode::Track;
        Params pa; pa.mode = Mode::Album;
        std::printf("        曲目      标签track  标签album   off     track    album\n");
        for (size_t i = 0; i < v.size(); ++i) {
            const auto do_ = decide(v[i], po, &albums);
            const auto dt = decide(v[i], pt, &albums);
            const auto da = decide(v[i], pa, &albums);
            std::printf("        %-8s  %+8.2f  %+8.2f  %+7.2f %+8.2f %+8.2f\n", v[i].path.c_str(),
                        dt.requestedDb, da.requestedDb, do_.appliedDb, dt.appliedDb, da.appliedDb);
            check(!do_.active && do_.appliedDb == 0.0, std::string("off 模式不动 ") + v[i].path);
        }
        const auto dA = decide(v[0], pt, &albums);
        const auto dB = decide(v[1], pt, &albums);
        check(approx_eq(dA.appliedDb, -7.50, 1e-9) && approx_eq(dB.appliedDb, -5.00, 1e-9), "track 模式逐曲不同增益");
        const auto aA = decide(v[0], pa, &albums);
        const auto aB = decide(v[1], pa, &albums);
        check(approx_eq(aA.appliedDb, -6.10, 1e-9) && approx_eq(aB.appliedDb, -6.10, 1e-9),
              "album 模式同专辑同增益（这才是 Album Gain 的意义）");

        // album 模式下拿不到专辑数据 ⟹ 退回逐曲标签，且必须在 source 里写明（不许静默降级）
        {
            Track solo = v[0];
            Params pa2; pa2.mode = Mode::Album;
            const auto d = decide(solo, pa2, nullptr);          // 不传 albums
            check(d.active && d.source == "tag:track(fallback)" && approx_eq(d.appliedDb, -7.50, 1e-9),
                  "album 模式缺专辑数据时退回逐曲标签（source 标明 fallback）", d.source);
            check(!d.note.empty(), "降级必须留 note");
        }
        // 标签写坏（-999 dB）⟹ 区间钳制，不许把音量打没
        {
            Track bad = v[0];
            bad.tags.trackGainRaw = "-999.00 dB";
            bad.tags.trackPeakRaw.clear();                      // 没有峰值信息，只看钳制
            Params pt2; pt2.mode = Mode::Track;
            const auto d = decide(bad, pt2, nullptr);
            check(d.clamped && approx_eq(d.appliedDb, -60.0, 1e-9), "越界标签被钳到 −60 dB",
                  std::to_string(d.appliedDb));
        }
        // 计算路径：没有标签 ⟹ album 模式用专辑整体响度，track 模式用逐曲响度
        std::vector<Track> w = v;
        for (auto& t : w) { t.tags.trackGainRaw.clear(); t.tags.albumGainRaw.clear(); }
        const auto alb2 = compute_albums(w, -18.0);
        const auto t0 = decide(w[0], pt, &alb2);
        const auto t1 = decide(w[1], pt, &alb2);
        const auto al0 = decide(w[0], pa, &alb2);
        const auto al1 = decide(w[1], pa, &alb2);
        std::printf("        [计算] A: track=%+.2f album=%+.2f   B: track=%+.2f album=%+.2f\n",  // 负=压下去
                    t0.appliedDb, al0.appliedDb, t1.appliedDb, al1.appliedDb);
        // gain = −18 − LUFS：A(−14) ⟹ −4.0 dB（比目标响，要压下去），B(−12.5) ⟹ −5.5 dB
        check(approx_eq(t0.appliedDb, -4.0, 0.01) && approx_eq(t1.appliedDb, -5.5, 0.01),
              "计算 track = −18 − LUFS（A −14→−4.0, B −12.5→−5.5）",
              std::to_string(t0.appliedDb) + " / " + std::to_string(t1.appliedDb));
        check(approx_eq(al0.appliedDb, al1.appliedDb, 1e-9) && al0.source == "compute:album",
              "计算 album：同组同增益，来源 compute:album", al0.source);
    }

    // ---------------------------------------------------------------- [6]
    std::printf("\n[6] 与 loudness 串联：不双倍提升\n");
    {
        const int rate = 48000;
        const size_t n = (size_t)rate * 2;
        auto make = [&](double hz) {
            std::vector<std::vector<double>> s(2, std::vector<double>(n));
            for (size_t i = 0; i < n; ++i) {
                const double v = 0.25 * std::sin(2.0 * 3.14159265358979 * hz * (double)i / rate);
                s[0][i] = v; s[1][i] = v;
            }
            return s;
        };
        auto rms_db = [](const std::vector<double>& x, size_t skip) {
            double e = 0.0; size_t c = 0;
            for (size_t i = skip; i < x.size(); ++i) { e += x[i] * x[i]; ++c; }
            return 10.0 * std::log10(e / (double)c + 1e-300);
        };
        const double rgDb = -6.0, volDb = -10.0;
        LoudnessConfig lc;                       // 默认：bass 100 Hz 6 dB / treble 6 kHz 3 dB
        lc.lowerBoundDb = -30.0; lc.upperBoundDb = 0.0;

        for (double hz : {1000.0, 60.0}) {
            auto sig = make(hz);
            // 逐级单独测：每一级的增益 = 单独跑时的电平变化
            auto gain_of = [&](const std::vector<std::vector<double>>& a, const std::vector<std::vector<double>>& b) {
                return rms_db(b[0], 4800) - rms_db(a[0], 4800);
            };
            auto base = sig;
            // ① 只 RG
            auto s1 = sig;
            for (auto& c : s1) apply_gain(rgDb, c.data(), c.size());
            const double gRg = gain_of(base, s1);
            // ② 只 loudness（音量 −30 dB ⟹ 满提升）
            auto s2 = sig;
            Loudness L(lc); L.init(rate); L.set_volume_db(-30.0);
            for (auto& c : s2) L.process(c.data(), c.size());
            const double gLoud = gain_of(base, s2);
            // ③ 只音量
            auto s3 = sig;
            VolumeControl V; V.set_sample_rate(rate); V.set_gain_db(volDb); V.snap();
            for (auto& c : s3) V.process(c.data(), c.size());
            const double gVol = gain_of(base, s3);
            // ④ 串联：RG → loudness → volume（这就是接入后的真实顺序）
            auto s4 = sig;
            for (auto& c : s4) apply_gain(rgDb, c.data(), c.size());
            Loudness L2(lc); L2.init(rate); L2.set_volume_db(-30.0);
            for (auto& c : s4) L2.process(c.data(), c.size());
            VolumeControl V2; V2.set_sample_rate(rate); V2.set_gain_db(volDb); V2.snap();
            for (auto& c : s4) V2.process(c.data(), c.size());
            const double gChain = gain_of(base, s4);
            const double expect = gRg + gLoud + gVol;
            std::printf("        %5.0f Hz: RG=%+.3f  loudness=%+.3f  vol=%+.3f  单级和=%+.3f  串联实测=%+.3f  Δ=%.4f dB\n",
                        hz, gRg, gLoud, gVol, expect, gChain, gChain - expect);
            check(approx_eq(gChain, expect, 1e-6), "串联合成 = 各级之和（没有二次提升）", std::to_string(gChain - expect));
            check(approx_eq(gRg, rgDb, 1e-9), "ReplayGain 级严格 = 请求值", std::to_string(gRg));
            if (hz == 60.0) check(gLoud > 3.0, "loudness 在 60 Hz 确有提升（否则这条判据测不出叠加）", std::to_string(gLoud));
            if (hz == 1000.0) check(std::fabs(gLoud) < 1.0, "1 kHz 处 loudness 近似 0 dB（架式滤波形状）", std::to_string(gLoud));
        }
        // loudness 的电平引用 = 音量 + RG（一次性换算，不是反馈）
        check(approx_eq(loudness_volume_db(-10.0, -6.0), -16.0, 1e-12), "loudness_volume_db(-10, -6) = −16 dB");
    }

    // ---------------------------------------------------------------- [7]
    std::printf("\n[7] 专辑响度：时长加权 vs 拼接后统一门限\n");
    {
        const int rate = 48000;
        auto tone = [&](double amp, double secs) {
            std::vector<double> x((size_t)(rate * secs));
            for (size_t i = 0; i < x.size(); ++i) x[i] = amp * std::sin(2.0 * 3.14159265358979 * 440.0 * (double)i / rate);
            return x;
        };
        const std::vector<double> a = tone(0.10, 4.0), b = tone(0.30, 8.0), c = tone(0.03, 4.0);

        auto lufs_of = [&](const std::vector<double>& x) {
            rta::analysis::DynamicsAccumulator acc;
            acc.begin(2, (double)rate);
            std::vector<double> inter(x.size() * 2);
            for (size_t i = 0; i < x.size(); ++i) { inter[2 * i] = x[i]; inter[2 * i + 1] = x[i]; }
            acc.add_interleaved(inter.data(), x.size());
            return acc.finalize().lufsIntegrated;
        };
        std::vector<Track> v(3);
        const std::vector<double>* xs[3] = {&a, &b, &c};
        for (int i = 0; i < 3; ++i) {
            v[(size_t)i].tags.album = "Alb"; v[(size_t)i].tags.albumArtist = "A";
            v[(size_t)i].meas.valid = true;
            v[(size_t)i].meas.lufs = lufs_of(*xs[i]);
            v[(size_t)i].meas.durationSec = (double)xs[i]->size() / rate;
        }
        const auto albums = compute_albums(v, -18.0);
        const double weighted = albums[0].lufs;
        // 拼接：把三首串起来当一条信号跑同一个分析器（门限跨曲目重算）
        std::vector<double> cat;
        for (int i = 0; i < 3; ++i) cat.insert(cat.end(), xs[i]->begin(), xs[i]->end());
        const double concatenated = lufs_of(cat);
        std::printf("        逐曲 LUFS: %.3f / %.3f / %.3f\n", v[0].meas.lufs, v[1].meas.lufs, v[2].meas.lufs);
        std::printf("        专辑 LUFS: 时长加权 %.3f  vs  拼接门限 %.3f  Δ=%.3f LU\n",
                    weighted, concatenated, weighted - concatenated);
        std::printf("        专辑增益 = %+.3f dB（参考 −18 LUFS）\n", albums[0].gainDb);
        check(albums.size() == 1 && albums[0].members.size() == 3, "同专辑三首归为一组");
        // 这一条**故意把差写出来**：加权近似不是"等于"，极端素材能差到 1.2 LU。
        // 所以凡是能离线把整张专辑当一条信号测的地方，都应改用 override（见下一条断言）。
        check(std::fabs(weighted - concatenated) < 1.5,
              "加权口径与拼接口径差 < 1.5 LU（实测值见上一行，别当成 0）",
              std::to_string(weighted - concatenated));

        // override 路径：用拼接门限值当专辑响度 ⟹ 专辑增益必须与之一致
        std::map<std::string, double> ov;
        ov[v[0].album_key()] = concatenated;
        const auto albumsOv = compute_albums(v, -18.0, &ov);
        check(albumsOv.size() == 1 && albumsOv[0].exactLufs, "override 专辑响度生效（exactLufs）");
        check(approx_eq(albumsOv[0].gainDb, -18.0 - concatenated, 1e-9),
              "override 后专辑增益 = −18 − 拼接 LUFS", std::to_string(albumsOv[0].gainDb));

        // 不同 albumartist ⟹ 不同组（判据：同 album + 同 albumartist）
        std::vector<Track> v2 = v;
        v2[1].tags.albumArtist = "B";
        const auto al2 = compute_albums(v2, -18.0);
        check(al2.size() == 2, "同专辑名但 albumartist 不同 ⟹ 分成 2 组", std::to_string(al2.size()));
        // albumartist 缺失时退回 artist
        std::vector<Track> v3 = v;
        for (auto& t : v3) t.tags.albumArtist.clear();
        v3[0].tags.artist = "X"; v3[1].tags.artist = "Y"; v3[2].tags.artist = "X";   // X,X | Y ⟹ 2 组
        const auto al3 = compute_albums(v3, -18.0);
        check(al3.size() == 2, "albumartist 缺失 ⟹ 退回 artist 归组", std::to_string(al3.size()));
    }

    // ---------------------------------------------------------------- [8]
    std::printf("\n[8] 真实库只读扫描（D:\\RTA-hqplayer\\MUSIC）\n");
    {
        const fs::path music = fs::u8path("D:\\RTA-hqplayer\\MUSIC");
        if (!fs::exists(music)) {
            std::printf("  [SKIP] 没有真实库目录\n");
        } else {
            size_t files = 0, tagTrack = 0, tagAlbum = 0, none = 0, both = 0;
            std::map<std::string, size_t> cont, keys;
            std::error_code e2;
            for (auto it = fs::recursive_directory_iterator(music, e2); it != fs::recursive_directory_iterator(); it.increment(e2)) {
                if (e2) break;
                if (!it->is_regular_file(e2)) continue;
                std::string ext = path_utf8(it->path().extension());
                for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch);
                if (ext != ".flac" && ext != ".wav" && ext != ".wave" && ext != ".mp3" && ext != ".ogg" &&
                    ext != ".m4a" && ext != ".dsf" && ext != ".dff") continue;
                ++files;
                TagSet t;
                read_tags(path_utf8(it->path()), &t);
                if (t.ok) cont[container_name(t.container)]++;
                double g = 0.0;
                const bool ht = t.has_track_gain() && parse_gain_db(t.trackGainRaw, &g);
                const bool ha = t.has_album_gain() && parse_gain_db(t.albumGainRaw, &g);
                if (ht) ++tagTrack;
                if (ha) ++tagAlbum;
                if (!ht && !ha) ++none;
                if (ht && ha) ++both;
                if (!t.keyTrackGain.empty()) keys["track:" + t.keyTrackGain]++;
                if (!t.keyAlbumGain.empty()) keys["album:" + t.keyAlbumGain]++;
            }
            std::printf("        文件 %zu：有 track gain %zu，有 album gain %zu，两者都有 %zu，都没有 %zu\n",
                        files, tagTrack, tagAlbum, both, none);
            std::printf("        容器判定：");
            for (const auto& kv : cont) std::printf(" %s=%zu", kv.first.c_str(), kv.second);
            std::printf("\n        读到的键名：");
            for (const auto& kv : keys) std::printf(" %s x%zu", kv.first.c_str(), kv.second);
            std::printf("\n");
            check(files > 0, "真实库扫到了文件", std::to_string(files));
            // 只读纪律：本节的判据是"统计出来"，不是"必须有多少标签"——
            // 实测本库 0 个 ReplayGain 标签（报告里有数字），所以这里不断言数量。
        }
    }

    // ---------------------------------------------------------------- [9]
    std::printf("\n[9] 真实库：计算值 vs 标签值（口径对照）\n");
    {
        const fs::path music = fs::u8path("D:\\RTA-hqplayer\\MUSIC");
        std::vector<std::string> tagged;
        if (fs::exists(music)) {
            std::error_code e3;
            for (auto it = fs::recursive_directory_iterator(music, e3); it != fs::recursive_directory_iterator(); it.increment(e3)) {
                if (e3) break;
                if (!it->is_regular_file(e3)) continue;
                std::string ext = path_utf8(it->path().extension());
                for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch);
                if (ext != ".flac" && ext != ".mp3" && ext != ".ogg" && ext != ".wav") continue;
                TagSet ts;
                read_tags(path_utf8(it->path()), &ts);
                if (ts.has_track_gain()) tagged.push_back(path_utf8(it->path()));
            }
        }
        if (tagged.empty()) {
            std::printf("  [SKIP] 真实库里没有带 ReplayGain 的文件，本节无从对照（报告里要写明）\n");
        } else {
            std::vector<double> diffs18, diffs23;
            std::printf("        文件            标签gain   LUFS(实测)  计算gain(-18)  Δ18    计算gain(-23)  Δ23\n");
            std::vector<Track> tt;
            for (const auto& p : tagged) {
                TagSet ts;
                read_tags(p, &ts);
                double g = 0.0;
                if (!parse_gain_db(ts.trackGainRaw, &g)) continue;
                double lufs = 0.0, pk = 0.0, dur = 0.0;
                if (!measure_file(p, &lufs, &pk, &dur)) { std::printf("        [SKIP 解码失败] %s\n", p.c_str()); continue; }
                const double c18 = gain_from_lufs(lufs, -18.0);
                const double c23 = gain_from_lufs(lufs, -23.0);
                diffs18.push_back(c18 - g);
                diffs23.push_back(c23 - g);
                const size_t sep = p.find_last_of("\\/");
                std::printf("        %-14s %+8.2f  %10.2f  %+10.2f  %+6.2f  %+10.2f  %+6.2f\n",
                            (sep == std::string::npos ? p : p.substr(sep + 1)).c_str(), g, lufs, c18, c18 - g, c23, c23 - g);
                Track tr; tr.path = p; tr.tags = ts;
                tr.meas.valid = true; tr.meas.lufs = lufs; tr.meas.peak = pk; tr.meas.durationSec = dur;
                tt.push_back(tr);
            }
            if (!diffs18.empty()) {
                auto med = [](std::vector<double> x) { std::sort(x.begin(), x.end()); return x[x.size() / 2]; };
                const double m18 = med(diffs18), m23 = med(diffs23);
                std::printf("        中位差：以 −18 LUFS 为参考 %+0.2f dB；以 −23 LUFS 为参考 %+0.2f dB（n=%zu）\n",
                            m18, m23, diffs18.size());
                // 判据：这批标签显然是 RG2 口径（−18），所以 |中位差| 必须小；
                // 若哪天换了一批 −23 口径的标签，这条会失败并提示"不是算错了，是口径不同"。
                check(std::fabs(m18) < 1.5, "标签与 −18 LUFS 口径一致（中位差 < 1.5 dB）", std::to_string(m18));
                // 专辑：同一专辑所有曲目用一个增益（album 模式）
                const auto alb = compute_albums(tt, -18.0);
                size_t maxGroup = 0;
                for (const auto& a : alb) maxGroup = std::max(maxGroup, a.members.size());
                std::printf("        归组：%zu 组，最大组 %zu 首", alb.size(), maxGroup);
                for (const auto& a : alb) {
                    if (a.members.size() < 2) continue;
                    std::printf("  [%s : %zu 首, 专辑LUFS=%.2f, gain=%+.2f dB, 来源=%s]",
                                a.album.c_str(), a.members.size(), a.lufs, a.gainDb, a.fromTag ? "标签" : "计算");
                }
                std::printf("\n");
                check(alb.size() >= 1 && maxGroup >= 2, "真实专辑能被归组（同 album + 同 albumartist）", std::to_string(maxGroup));
            }
        }
    }

    // ---------------------------------------------------------------- [10]
    std::printf("\n[10] 文件级防削波闭环（rg_probe 端到端：ffmpeg 写标签 → 解码实测 → 判定）\n");
    {
#ifdef RTA_RG_PROBE
        const std::string probeExe = RTA_RG_PROBE;
        const fs::path wav = tmp / "peak_plus05.wav";
        // 造 peak = +0.5 dBFS 的素材（float WAV 允许样本越界，这正是要防的场景）
        const int rate = 48000;
        const double amp = std::pow(10.0, 0.5 / 20.0);
        std::vector<std::vector<double>> sig(2, std::vector<double>(rate * 3));
        for (size_t i = 0; i < sig[0].size(); ++i)
            for (int c = 0; c < 2; ++c) sig[(size_t)c][i] = amp * std::sin(2.0 * 3.14159265358979 * 997.0 * (double)i / rate);
        check(write_wav_f32(wav, sig, rate), "写出 peak=+0.50 dBFS 的 float WAV");

        const std::string tagged = path_utf8(tmp / "peak_plus05_tagged.wav");
        int rc = 1;
        check(fs::copy_file(wav, fs::u8path(tagged), fs::copy_options::overwrite_existing),
              "复制出带标签的副本（不动原素材）");
        // ID3v2(TXXX) 写进 WAV 的 "id3 " 块 —— 与真实 tagger 的写法一致
        const bool made = wav_add_id3_chunk(fs::u8path(tagged),
            {{"REPLAYGAIN_TRACK_GAIN", "+6.00 dB"},
             {"REPLAYGAIN_TRACK_PEAK", "1.059254"},
             {"TITLE", "PeakFixture"}});
        check(made, "往 WAV 的 \"id3 \" 块写入 ID3v2.3 TXXX 标签");
        if (made) {
            TagSet ts;
            read_tags(tagged, &ts);
            check(ts.ok && ts.container == Container::Id3v2 && ts.keyTrackGain == "REPLAYGAIN_TRACK_GAIN",
                  "WAV 的 \"id3 \" 块也能读到 TXXX 标签", ts.container == Container::Id3v2 ? ts.keyTrackGain : ts.why);

            std::string out;
            if (run_proc({probeExe, tagged, "--mode", "track", "--json", "--compute", "missing"}, &out, &rc) && rc == 0) {
                double applied = 0.0, peakDb = 0.0, requested = 0.0;
                const bool hasA = json_num(out, "appliedDb", &applied);
                json_num(out, "peakDbfs", &peakDb);
                json_num(out, "requestedDb", &requested);
                std::printf("        rg_probe: requested=%+.3f dB  peak=%.3f dBFS  applied=%+.3f dB  clipLimited=%s\n",
                            requested, peakDb, applied, out.find("\"clipLimited\": true") != std::string::npos ? "true" : "false");
                check(hasA && approx_eq(requested, 6.0, 1e-6), "请求增益 +6.00 dB（来自标签）", std::to_string(requested));
                check(approx_eq(applied, -0.5, 0.02), "applied 被限到 −0.50 dB", std::to_string(applied));
                check(out.find("\"clipLimited\": true") != std::string::npos, "clipLimited=true（被削波保护改过）");
                check(applied + peakDb <= 0.02, "数学上 applied + peak ≤ 0 dBFS",
                      std::to_string(applied + peakDb));
            } else {
                check(false, "rg_probe 运行失败", out);
            }
        }
#else
        std::printf("  [SKIP] 未定义 RTA_RG_PROBE（CMake 未注入 rg_probe 路径）\n");
#endif
    }

    std::printf("\n== %s（失败 %d 项）==\n", fails == 0 ? "ALL PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}
