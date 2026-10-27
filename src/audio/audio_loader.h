// audio_loader.h — 统一音频读取入口
//
// WAV 走原生解析（wav.h），保持逐位精确、零依赖。
// 其他格式（FLAC / ALAC / MP3 / AAC / OGG / Opus / WMA / DSF / DFF ...）
// 交给系统上已有的 ffmpeg 解码成【对应位深的 PCM WAV】，再用原生 WAV 读取。
//
// 为什么这样做：
//   * 解码是确定性过程。无损格式（FLAC/ALAC/WavPack/APE）解出来的 PCM 与原文件
//     携带的 PCM 完全一致 —— 因此不会破坏"比特完美"这一核心主张。
//   * 不需要引入任何第三方库的二进制或授权问题。
//   * ffmpeg 对每种格式都是参考实现级别的正确性。
//
// 位深处理：先探测源的真实位深，再用 pcm_s16le / pcm_s24le / pcm_s32le 请求，
//   避免把 24-bit 素材降成 16-bit，也避免把 16-bit 素材无谓地抬成 24-bit。
//   ★ 2026-10-27（修 D7）：解码器输出是【浮点】的格式（mp3/aac/vorbis/opus/wma/dsd…）
//   改用 pcm_f32le 中转 —— 任何整数格式（s16/s24/s32 一样）都会把 >0 dBFS 的过冲钳到满幅，
//   实测 320k MP3 的 +0.66 dBFS 过冲被压成精确 1.0（峰值低 0.66 dB + 假平顶削波）。
//   整数源（flac/alac/wavpack/pcm_*…）仍走整数中转 ⟹ 无损格式解码结果与以前逐位相同。
//   详见 probe_ffmpeg_pcm() 的注释。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "audio/wav.h"
#include "audio/dsd_reader.h"
#include "dsp/dsd_integrator.h"
#include <string>
#include <vector>
#include <sstream>
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#include <new>
#include <chrono>
#include <thread>
#include <cstring>

namespace rta::audio {

// 不弹窗地跑一个命令，返回进程退出码（-1 表示启动失败）
inline int run_hidden(const std::wstring& cmdline, std::string* captured = nullptr) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (captured && !CreatePipe(&rd, &wr, &sa, 0)) return -1;
    if (wr) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (captured) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError  = wr;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    std::wstring tmp = cmdline;                 // CreateProcessW 需要可写缓冲
    if (!CreateProcessW(nullptr, tmp.data(), nullptr, nullptr, captured ? TRUE : FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (captured) { CloseHandle(rd); CloseHandle(wr); }
        return -1;
    }
    if (captured) CloseHandle(wr);
    if (captured) {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0)
            captured->append(buf, got);
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

inline std::string lowerExt(const std::string& p) {
    const size_t d = p.find_last_of('.');
    if (d == std::string::npos) return "";
    std::string e = p.substr(d);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

// ===========================================================================
// ISO 镜像识别（2026-10-27，用户 261027-01 #3）
//
// 为什么需要：`.iso` 以前连「识别」都没有（扩展名白名单里没有它），用户把 SACD ISO 拖进来
// 只会看到 ffmpeg 的 "Invalid data found when processing input"。真正的解决要写 SACD 解析，
// 这里只做**识别**这一档：分清「SACD 镜像」与「普通数据盘」，然后给出明确指引。
//
// 判据（只用 ISO9660 的结构，不猜内容）：
//   ① 扇区 16（偏移 32768）起的卷描述符标识必须是 "CD001"，且能找到类型 1（主卷描述符）；
//   ② 根目录记录（PVD 偏移 156，34 字节）给出根目录起始 LBA 与长度；
//   ③ 在根目录里找名字含 "SACD" 的目录/文件（SACD 母盘在 ISO9660 里就是一个 SACD/ 目录，
//      内含 TOC 与 2ch/mch 音轨区）；卷标（PVD 偏移 40，32 字节）也一并看。
// SACD ISO 有 ISO9660 层，所以这套判据对 sacd_extract 产出的镜像成立；真 UDF-only 镜像会落到
// 「无法识别」而不是被误判成 SACD —— 宁可说不知道，也不给错指引。
// ===========================================================================
enum class IsoKind {
    NotIso,    // 打不开 / 不是 ISO9660 镜像
    Sacd,      // 根目录（或卷标）里有 SACD 字样 ⟹ SACD 镜像
    Data       // 是 ISO9660 镜像，但没有 SACD 目录 ⟹ 普通数据盘
};

struct IsoInfo {
    IsoKind kind = IsoKind::NotIso;
    std::string volumeId;     // PVD 卷标（去掉尾部空格）
    std::string why;          // 给用户看的判断依据（卷标/根目录命中项/失败原因）
};

// 只读前 4 个卷描述符扇区；需要时才读根目录扇区。全程只读、无子进程、不开任何设备。
inline IsoInfo iso_inspect(const std::string& path) {
    IsoInfo out;
    FILE* f = dsd_detail::dsd_fopen(path, L"rb", "rb");
    if (!f) { out.why = "打不开文件"; return out; }
    struct Guard { FILE* f; ~Guard() { if (f) std::fclose(f); } } g{ f };

    // ---- ① 卷描述符：16 号扇区起连续若干张，找到类型 1（主卷描述符）为止 ----
    uint8_t vd[2048];
    bool havePvd = false;
    for (int s = 16; s < 20; ++s) {
        if (_fseeki64(f, (long long)s * 2048, SEEK_SET) != 0) break;
        if (std::fread(vd, 1, sizeof(vd), f) != sizeof(vd)) break;
        if (std::memcmp(vd + 1, "CD001", 5) != 0) break;   // 不是 ISO9660
        if (vd[0] == 0x01) { havePvd = true; break; }
        if (vd[0] == 0xFF) break;                          // 终止描述符
    }
    if (!havePvd) { out.why = "没有 ISO9660 主卷描述符"; return out; }

    out.volumeId.assign((const char*)vd + 40, 32);
    while (!out.volumeId.empty() && (out.volumeId.back() == ' ' || out.volumeId.back() == '\0'))
        out.volumeId.pop_back();

    // ---- ② 根目录记录：PVD 偏移 156 起 34 字节（extent LBA 小端在前 4 字节） ----
    const uint8_t* rr = vd + 156;
    const uint32_t rootLba = (uint32_t)rr[2] | ((uint32_t)rr[3] << 8) |
                             ((uint32_t)rr[4] << 16) | ((uint32_t)rr[5] << 24);
    const uint32_t rootLen = (uint32_t)rr[10] | ((uint32_t)rr[11] << 8) |
                             ((uint32_t)rr[12] << 16) | ((uint32_t)rr[13] << 24);
    out.kind = IsoKind::Data;                              // 到这里已是合法 ISO9660 镜像
    out.why = "ISO9660 卷标=" + (out.volumeId.empty() ? std::string("(空)") : out.volumeId);

    // ---- ③ 根目录：逐条目录记录找 SACD（大小写不敏感；带 ";1" 版本后缀的也认） ----
    // 名字长度 0 = 到本扇区末尾（填充），直接跳下一个扇区；长度 < 34 视为坏记录，停。
    uint8_t dir[2048];
    std::string hits;
    const uint32_t maxSectors = rootLen ? (rootLen + 2047u) / 2048u : 1u;
    for (uint32_t s = 0; s < maxSectors && s < 8; ++s) {
        if (_fseeki64(f, (long long)(rootLba + s) * 2048, SEEK_SET) != 0) break;
        if (std::fread(dir, 1, sizeof(dir), f) != sizeof(dir)) break;
        size_t off = 0;
        while (off + 33 < sizeof(dir)) {
            const uint8_t len = dir[off];
            if (len == 0) break;                       // 本扇区剩余为填充
            if (len < 34 || off + len > sizeof(dir)) break;
            const uint8_t nameLen = dir[off + 32];
            if (nameLen > 0 && (size_t)off + 33 + nameLen <= sizeof(dir)) {
                const std::string nm((const char*)dir + off + 33, nameLen);
                std::string up = nm;
                for (auto& c : up) c = (char)std::toupper((unsigned char)c);
                if (up.find("SACD") != std::string::npos) {
                    if (hits.size() < 120) { if (!hits.empty()) hits += " "; hits += nm; }
                    out.kind = IsoKind::Sacd;
                }
            }
            off += len;
        }
    }
    if (out.kind == IsoKind::Sacd) {
        out.why = "根目录含 " + hits;
    } else {
        std::string up = out.volumeId;
        for (auto& c : up) c = (char)std::toupper((unsigned char)c);
        if (up.find("SACD") != std::string::npos) {
            out.kind = IsoKind::Sacd;
            out.why = "卷标含 SACD（" + out.volumeId + "）";
        }
    }
    return out;
}

// 播放/解码入口对 .iso 的统一处理：给出「SACD 需转换」或「这不是音频镜像」，绝不落到 ffmpeg。
[[noreturn]] inline void throw_for_iso(const std::string& path) {
    const IsoInfo ii = iso_inspect(path);
    if (ii.kind == IsoKind::Sacd)
        throw UnsupportedFormat(msg_sacd_iso_unsupported() + "（文件=" + path + "，" + ii.why + "）");
    if (ii.kind == IsoKind::Data)
        throw UnsupportedFormat(msg_iso_not_audio() + "（文件=" + path + "，" + ii.why + "）");
    throw UnsupportedFormat(
        "这个 .iso 里没有 ISO9660 文件系统，无法判断是不是 SACD 镜像（" + ii.why + "）。"
        "如果是 SACD ISO，请先用 sacd_extract 转成分轨 .dsf 后入库。");
}

// 播放器所在目录（分发时把 ffmpeg.exe / ffprobe.exe 放在这里就能自带解码器，
// 不需要改 PATH）。这也是"环境齐全"的关键：程序能找到它，用户不用配环境变量。
inline std::wstring exe_dir_w() {
    wchar_t p[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (n == 0) return L"";
    std::wstring s(p, n);
    const size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : s.substr(0, slash);
}

inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    const int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), m, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// 找到 ffmpeg / ffprobe：先看 exe 同目录（自带），再退回 PATH。
inline std::string find_tool(const char* name) {
    const std::wstring dir = exe_dir_w();
    if (!dir.empty()) {
        const std::wstring local = dir + L"\\" + widen(name) + L".exe";
        if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES)
            return wide_to_utf8(local);
    }
    wchar_t buf[4096];
    const std::wstring w = widen(name);
    const DWORD n = SearchPathW(nullptr, w.c_str(), L".exe", 4096, buf, nullptr);
    if (n == 0 || n >= 4096) return "";
    return wide_to_utf8(std::wstring(buf, n));
}

// 源文件「当下能不能被读」自检（2026-10-22 现场 BUG：ffmpeg 报 Illegal byte sequence / -42）
//
// 为什么需要这一步：ffmpeg 打开输入失败后会【回退到 ANSI 代码页】再试一次，含非 ASCII 字符的
// 文件在该回退里被 MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS) 判成非法序列，于是把
// ENOENT / EACCES / 共享冲突统统报成 EILSEQ（-42 "Illegal byte sequence"）——
// 实测：同一个日文名文件，存在时 exit=0；被独占占用时 exit=-42；而纯 ASCII 名同样被占用时
// 如实报 exit=-13。也就是说 -42 是【掩码】，不是真原因。
// 所以真实原因必须由我们自己问 Win32 拿：这里用一次带全共享的 CreateFileW 探一下。
// 返回空串 = 此刻可读；非空 = 人话原因（含 Win32 错误码），可直接进错误文本。
inline std::string source_read_probe(const std::string& path) {
    const std::wstring w = widen(path);
    if (w.empty()) return "路径无法转成宽字符（不是合法 UTF-8）";
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        std::string why;
        switch (e) {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
                why = "源文件此刻不存在（可能正在被移动/标签写回/删除，或刚被改名）";
                break;
            case ERROR_SHARING_VIOLATION:
            case ERROR_LOCK_VIOLATION:
                why = "源文件正被其它程序独占占用（标签编辑器、同步客户端、杀毒扫描都会这样）";
                break;
            case ERROR_ACCESS_DENIED:
                why = "没有读取权限（ACL / 只读介质 / 受保护目录）";
                break;
            default:
                why = "无法打开";
                break;
        }
        return why + "，Win32 错误 " + std::to_string((unsigned)e);
    }
    CloseHandle(h);
    return "";
}

// 用 ffprobe 探测源文件的真实位深，返回 16/24/32；失败返回 24
inline int probe_bit_depth(const std::string& ffprobe, const std::string& file) {
    if (ffprobe.empty()) return 24;
    std::string out;
    const std::wstring cmd = L"\"" + widen(ffprobe) + L"\" -v error -select_streams a:0 "
                             L"-show_entries stream=bits_per_raw_sample -of default=nw=1:nk=1 \"" + widen(file) + L"\"";
    if (run_hidden(cmd, &out) != 0) return 32;
    // 有损格式（MP3/AAC/OGG/Opus）的 bits_per_raw_sample 是 N/A，atoi 得 0。
    // 这种情况绝不能当成 16 bit：解码器内部是 float，提前量化到 16 bit 会引入
    // 实测 -101 dBFS RMS 的量化误差（峰值 -96 dBFS），而且是在 DSP 之前。
    // 未知一律按 32 bit 请求，让量化只发生在最后按用户选择的位深做抖动时。
    const int v = atoi(out.c_str());
    if (v <= 0) return 32;
    if (v <= 16) return 16;
    if (v <= 24) return 24;
    return 32;
}

// ===========================================================================
// ffmpeg 中转的取样格式选择（2026-10-27 修 D7：整数中转把 >0 dBFS 的过冲钳成满幅）
//
// 现象（实测，见 build-qc/quality_confidence_report.md §2 / §4-D7）：
//   320k MP3《赤伶(交响乐版)》解码器输出峰值 = 1.0792（+0.662 dBFS），但旧代码请求
//   pcm_s32le，而 ffmpeg 的 flt→int 转换按【满幅】钳位（1.0792 × 2^31 溢出 int32），于是
//     ① 峰值偏低 0.662 dB（我们报 0.0000 dBFS，真值 +0.662 dBFS）；
//     ② 所有越限样本变成精确 1.0 ⟹ 波形出现"平顶"，看起来就是削波（制造假削波）。
//   s16/s24 同样会钳 —— 位深只决定量化误差，钳位点永远在 1.0。
//
// 修法：解码器输出是【浮点】（sample_fmt = flt/fltp/dbl/dblp）的格式一律用 pcm_f32le 中转，
//   全程不经过整数。为什么这不破坏逐位/bit-perfect 语义：
//   * 整数解码器（flac / alac / wavpack / tta / pcm_* …）**仍然请求整数格式**（16/24/32 原样），
//     WAV 与 DSD 的原生解析路径完全没动 ⟹ 无损格式的解码结果与以前逐位相同
//     （实测 11 首里 9 首峰值/RMS 指纹不变，只有 MP3 一项按预期变化）。
//   * 浮点中转对 ≤24 bit 整数是**精确**的（float32 有 24 位尾数，|v| ≤ 2^23 的整数一一对应），
//     所以即使某个整数源位深探测不到而落到 f32，16/24 bit 内容也不会掉一位；真正的 32 bit
//     整数源仍走 pcm_s32le（f32 存不下 32 bit 整数），绝不浮点化。
//   * 浮点缓冲 isFloat=true ⟹ pipeline::willBeBitPerfect() 本来就直接返回 false。有损源本来
//     就不该被标成"逐位直通"（s32 中转时代它反而可能被误标成 bit-perfect），这是修正。
// ===========================================================================
struct FfmpegPcmChoice {
    const char* codec = "pcm_f32le";
    bool        floatDecoder = false;  // 解码器输出浮点 ⟹ 峰值可能 > 0 dBFS（过冲）
    int         bits = 0;              // 整数解码器的位深（浮点源为 0）
    std::string why;                   // 人话原因（进 LoadNote / 日志，便于现场核对走了哪条）
};

// 用 ffprobe 探测 a:0 流的 sample_fmt + 位深，决定该向 ffmpeg 请求哪个 pcm_* 编码。
// 只读、无副作用、不开任何设备。探测失败或结果不可解析时**退回 pcm_f32le**：
//   f32 不会钳位，且对 ≤24 bit 整数精确 —— 比"猜一个整数位深"安全。
// 为什么看 sample_fmt 而不是只看位深：位深是 N/A 的正是有损格式（flt/fltp），而 pcm_s16le
//   这类整数编码的 bits_per_raw_sample 也是 N/A（要用 bits_per_sample），光看位深会误判。
inline FfmpegPcmChoice probe_ffmpeg_pcm(const std::string& ffprobe, const std::string& file) {
    FfmpegPcmChoice c;
    if (ffprobe.empty()) {
        c.why = "没有 ffprobe，无法判断解码器取样格式 ⟹ pcm_f32le（不会钳位）";
        return c;
    }
    std::string out;
    const std::wstring cmd = L"\"" + widen(ffprobe) + L"\" -v error -select_streams a:0 "
                             L"-show_entries stream=sample_fmt,bits_per_raw_sample,bits_per_sample "
                             L"-of default=nw=1 \"" + widen(file) + L"\"";
    if (run_hidden(cmd, &out) != 0) {
        c.why = "ffprobe 探测失败 ⟹ pcm_f32le（不会钳位）";
        return c;
    }
    std::string sfmt;
    int bprs = 0, bps = 0;
    {
        std::istringstream is(out);
        std::string line;
        while (std::getline(is, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t e = line.find('=');
            if (e == std::string::npos) continue;
            const std::string k = line.substr(0, e);
            const std::string v = line.substr(e + 1);
            if (k == "sample_fmt") sfmt = v;
            else if (k == "bits_per_raw_sample") bprs = (v == "N/A" || v.empty()) ? 0 : atoi(v.c_str());
            else if (k == "bits_per_sample")     bps  = (v == "N/A" || v.empty()) ? 0 : atoi(v.c_str());
        }
    }
    const bool flt = (sfmt.rfind("flt", 0) == 0) || (sfmt.rfind("dbl", 0) == 0);
    if (flt) {
        c.floatDecoder = true;
        c.codec = "pcm_f32le";
        c.why = "解码器输出浮点（sample_fmt=" + sfmt + "）⟹ pcm_f32le 中转，>0 dBFS 过冲不被钳位";
        return c;
    }
    // 整数解码器：flac/alac 用 bits_per_raw_sample，pcm_* 用 bits_per_sample
    const int d = bprs > 0 ? bprs : bps;
    if (d <= 0) {
        c.codec = "pcm_f32le";
        c.why = "整数解码器但位深未知（sample_fmt=" + sfmt + "）⟹ pcm_f32le（≤24 bit 精确）";
        return c;
    }
    c.bits = d;
    c.codec = (d <= 16) ? "pcm_s16le" : (d <= 24 ? "pcm_s24le" : "pcm_s32le");
    c.why = "整数解码器 " + sfmt + " @ " + std::to_string(d) + " bit ⟹ " + c.codec + "（与以前逐位相同）";
    return c;
}

struct LoadNote {
    std::string text;
    double ffmpegMs = 0.0;   // ffmpeg 转码写入临时 WAV 耗时 (ms)
    double wavReadMs = 0.0;  // 读取临时/原生 WAV 耗时 (ms)
    double totalMs = 0.0;    // 解码总耗时 (ms)
};

// DSD 源抽取设置（由 rtaweb / rtaplay 按配置写入；默认 FIR + narrow）
// 为什么用全局：read_any 的签名在很多地方被调用，加参数会波及媒体信息、库扫描与预解码槽。
// 代价是"播放线程与预解码线程可能短暂看到不同设置"，最多影响一首歌的开头，可接受。
struct DsdPcmSettings {
    dsp::IntegratorKind kind = dsp::IntegratorKind::Fir;
    dsp::DsdConversion  conversion = dsp::DsdConversion::Narrow;
    bool                twoStage = true;
};
inline DsdPcmSettings& dsd_pcm_settings() { static DsdPcmSettings s; return s; }

// 流式解码开关（ROADMAP P6b：免临时 WAV，默认 1 开启）
// 可通过设置键 streamdecode（/api/settings）、环境变量 RLPLAYER_STREAM_DECODE、
// 或 C++ set_stream_decode_enabled(bool) 进行控制，保持随时可回退。
struct StreamDecodeSettings {
    bool enabled = true;
};
inline StreamDecodeSettings& stream_decode_settings() { static StreamDecodeSettings s; return s; }
inline bool is_stream_decode_enabled() {
    const char* env = std::getenv("RLPLAYER_STREAM_DECODE");
    if (env && env[0] != '\0') {
        if (std::strcmp(env, "0") == 0 || std::strcmp(env, "false") == 0 || std::strcmp(env, "off") == 0)
            return false;
        if (std::strcmp(env, "1") == 0 || std::strcmp(env, "true") == 0 || std::strcmp(env, "on") == 0)
            return true;
    }
    return stream_decode_settings().enabled;
}
inline void set_stream_decode_enabled(bool en) {
    stream_decode_settings().enabled = en;
}

// 从匿名管道句柄读取 WAV 音频流并填充到 AudioBuffer（非阻塞读、分块拼装、逐位精确）
inline bool read_wav_stream_pipe(HANDLE hPipe, HANDLE hProcess, AudioBuffer& ab, std::string* errOut = nullptr) {
    std::vector<uint8_t> hbuf;
    WavHeaderInfo hdr{};
    bool hdrOk = false;
    constexpr size_t kMaxHeaderBytes = 1 << 20; // 1 MB 上限防异常
    const auto t_start = std::chrono::steady_clock::now();

    while (!hdrOk) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr)) {
            break;
        }
        if (avail > 0) {
            char chunk[4096];
            DWORD toRead = std::min<DWORD>(avail, (DWORD)sizeof(chunk));
            DWORD got = 0;
            if (ReadFile(hPipe, chunk, toRead, &got, nullptr) && got > 0) {
                hbuf.insert(hbuf.end(), (const uint8_t*)chunk, (const uint8_t*)chunk + got);
                std::string err;
                int r = parse_wav_header(hbuf.data(), hbuf.size(), hdr, &err);
                if (r > 0) {
                    hdrOk = true;
                    break;
                } else if (r < 0) {
                    if (errOut) *errOut = err;
                    return false;
                }
                if (hbuf.size() > kMaxHeaderBytes) {
                    if (errOut) *errOut = "WAV 头超过 1MB 防御上限";
                    return false;
                }
            }
        } else {
            if (WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
                DWORD finalAvail = 0;
                if (PeekNamedPipe(hPipe, nullptr, 0, nullptr, &finalAvail, nullptr) && finalAvail > 0) {
                    continue;
                }
                break;
            }
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t_start).count() > 30) {
                if (errOut) *errOut = "读取 WAV 头超时";
                return false;
            }
            Sleep(0);
        }
    }

    if (!hdrOk) {
        std::string err;
        int r = parse_wav_header(hbuf.data(), hbuf.size(), hdr, &err);
        if (r <= 0) {
            if (errOut && errOut->empty()) *errOut = err.empty() ? "WAV 头不完整或进程提前退出" : err;
            return false;
        }
    }

    ab.rate = hdr.rate;
    ab.channels = (int)hdr.channels;
    ab.bits = (int)hdr.bits;
    ab.isFloat = hdr.isFloat;
    const int bytesPer = (int)hdr.bits / 8;
    if (bytesPer <= 0 || ab.channels <= 0) {
        if (errOut) *errOut = "非法的 bits 或 channels";
        return false;
    }

    const size_t pcmStart = hdr.headerBytes;
    const size_t initPcmBytes = hbuf.size() > pcmStart ? (hbuf.size() - pcmStart) : 0;
    uint8_t leftover[4] = {0};
    size_t leftoverCount = 0;

    if (initPcmBytes > 0) {
        const size_t completeSamples = initPcmBytes / (size_t)bytesPer;
        ab.raw.resize(completeSamples);
        const uint8_t* p = hbuf.data() + pcmStart;
        for (size_t i = 0; i < completeSamples; ++i) {
            ab.raw[i] = decode_sample(p + i * (size_t)bytesPer, ab.bits, ab.isFloat);
        }
        leftoverCount = initPcmBytes % (size_t)bytesPer;
        if (leftoverCount > 0) {
            std::memcpy(leftover, p + completeSamples * (size_t)bytesPer, leftoverCount);
        }
    }
    hbuf.clear();
    hbuf.shrink_to_fit();

    std::vector<uint8_t> readChunk(65536);
    while (true) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr)) {
            break;
        }
        if (avail > 0) {
            DWORD toRead = std::min<DWORD>(avail, (DWORD)readChunk.size());
            DWORD got = 0;
            if (!ReadFile(hPipe, readChunk.data(), toRead, &got, nullptr) || got == 0) {
                break;
            }
            size_t chunkOff = 0;
            if (leftoverCount > 0) {
                const size_t needed = (size_t)bytesPer - leftoverCount;
                if ((size_t)got >= needed) {
                    std::memcpy(leftover + leftoverCount, readChunk.data(), needed);
                    ab.raw.push_back(decode_sample(leftover, ab.bits, ab.isFloat));
                    leftoverCount = 0;
                    chunkOff = needed;
                } else {
                    std::memcpy(leftover + leftoverCount, readChunk.data(), got);
                    leftoverCount += (size_t)got;
                    continue;
                }
            }
            const size_t rem = (size_t)got - chunkOff;
            const size_t numSamples = rem / (size_t)bytesPer;
            if (numSamples > 0) {
                const size_t oldSz = ab.raw.size();
                ab.raw.resize(oldSz + numSamples);
                const uint8_t* p = readChunk.data() + chunkOff;
                for (size_t i = 0; i < numSamples; ++i) {
                    ab.raw[oldSz + i] = decode_sample(p + i * (size_t)bytesPer, ab.bits, ab.isFloat);
                }
            }
            leftoverCount = rem % (size_t)bytesPer;
            if (leftoverCount > 0) {
                std::memcpy(leftover, readChunk.data() + chunkOff + numSamples * (size_t)bytesPer, leftoverCount);
            }
        } else {
            if (WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
                DWORD finalAvail = 0;
                if (PeekNamedPipe(hPipe, nullptr, 0, nullptr, &finalAvail, nullptr) && finalAvail > 0) {
                    continue;
                }
                break;
            }
            Sleep(0);
        }
    }
    return true;
}


// ---------------- DSD 流式解码（大文件；不整曲入内存） ----------------
//
// 为什么需要（2026-10-25 用户实测：384 s DSD64 / 271 MB）：
//   整曲原生解码要同时握住「1-bit 流 + 每声道 double 平面 + int32 缓冲」三层数据 ——
//   实测解码 4.9 s、缓冲 517 MB、进程峰值 1813 MB，起播前就先卡住、吃掉近 2 GB。
//   流式解码只保留「一个块」（DSF 默认 4 KB/声道）与抽取器状态：
//   起播延迟 = 一块的时间，内存 = 常数，与文件长度无关。
//
// 逐位一致性：与 read_dsd_native 使用同一个 DsdDecimator、同一组设置、同样的块边界
// （从第 0 位开始按块连续喂），因此 PCM 输出与整曲解码逐位相同 —— 测试里逐样本比对。
// 跳转（seek）会重建抽取器状态并从块边界重新开始；调用方按现有惯例预卷 0.5 s 丢弃，
// 抽取器瞬态因此落在被丢弃的区段里（与整曲缓冲 + seek_reset 的既有行为一致）。

struct DsdFileLayout {
    bool     isDsf = false;
    uint32_t rate = 0;                 // 1-bit 采样率
    int      channels = 0;
    uint64_t bitsPerChannel = 0;       // 每声道有效位数
    uint64_t bytesPerChannel = 0;      // 每声道字节数
    uint64_t dataOffset = 0;           // 数据区在文件内的绝对偏移
    uint64_t dataSize = 0;             // 数据区总字节
    uint32_t blockSize = 0;            // DSF：每声道块字节数；DFF：0（内部按 4 KB 读块）
    bool     paddedLastBlock = true;   // DSF 末块是否按整块补齐
    std::string container;             // "dsf" / "dff"
};

// 只读文件头（不载入数据）：给"要不要走流式"的判断与流式解码器用。
inline DsdFileLayout probe_dsd_layout(const std::string& path) {
    using namespace dsd_detail;
    const std::string ext = lowerExt(path);
    DsdFileLayout L;
    FILE* f = dsd_fopen(path, L"rb", "rb");
    if (!f) throw std::runtime_error("无法打开文件: " + path);
    struct Guard { FILE* f; ~Guard() { if (f) std::fclose(f); } } g{ f };
    auto rd = [&](uint64_t off, void* buf, size_t n) -> size_t {
        if (_fseeki64(f, (long long)off, SEEK_SET) != 0) return 0;
        return std::fread(buf, 1, n, f);
    };

    if (ext == ".dsf") {
        uint8_t h[28];
        if (rd(0, h, 28) != 28 || std::memcmp(h, "DSD ", 4) != 0)
            throw std::runtime_error("不是 DSF 文件（缺少 \"DSD \" 头）: " + path);
        if (rdl64(h + 4) != 28) throw std::runtime_error("DSF 头长度异常");
        uint64_t off = 28;
        bool haveFmt = false, haveData = false;
        uint32_t chNum = 0, rate = 0, blockSize = 0;
        uint64_t sampleCnt = 0;
        for (int guard = 0; guard < 64 && !haveData; ++guard) {
            uint8_t ch[12];
            if (rd(off, ch, 12) != 12) break;
            const uint64_t sz = rdl64(ch + 4);
            if (sz < 12) break;
            if (std::memcmp(ch, "fmt ", 4) == 0) {
                if (sz < 52) throw std::runtime_error("DSF fmt 块长度异常");
                std::vector<uint8_t> fb((size_t)sz);
                if (rd(off + 12, fb.data(), fb.size()) != fb.size())
                    throw std::runtime_error("DSF fmt 块被截断");
                const uint32_t fmtId = rdl32(fb.data() + 4);
                chNum     = rdl32(fb.data() + 12);
                rate      = rdl32(fb.data() + 16);
                const uint32_t bps = rdl32(fb.data() + 20);
                sampleCnt = rdl64(fb.data() + 24);
                blockSize = rdl32(fb.data() + 32);
                if (fmtId != 0) throw UnsupportedFormat(msg_dst_unsupported("DSF 流式解码，format id = " +
                                                        std::to_string(fmtId) + "，文件=" + path));
                if (bps != 1) throw UnsupportedFormat("DSF bits per sample 不是 1（不是标准 1-bit DSD，暂不支持）。"
                                                      "请转成标准 .dsf 再播放。（文件=" + path + "）");
                if (chNum < 1 || chNum > 8) throw std::runtime_error("DSF 声道数异常");
                if (rate < 44100) throw std::runtime_error("DSF 采样率异常");
                if (blockSize == 0 || blockSize > (1u << 20)) throw std::runtime_error("DSF 块长异常");
                if (sampleCnt == 0) throw std::runtime_error("DSF 样本数为 0");
                haveFmt = true;
            } else if (std::memcmp(ch, "data", 4) == 0) {
                L.dataOffset = off + 12;
                L.dataSize   = sz - 12;
                haveData = true;
                break;
            }
            off += (size_t)sz;
        }
        if (!haveFmt) throw std::runtime_error("DSF 缺少 fmt 块");
        if (!haveData) throw std::runtime_error("DSF 缺少 data 块");
        const uint64_t needBytes  = (sampleCnt + 7) / 8;
        const uint64_t fullBlocks = needBytes / blockSize;
        const uint64_t remBytes   = needBytes % blockSize;
        const uint64_t needData   = (fullBlocks + (remBytes ? 1 : 0)) * (uint64_t)chNum * blockSize;
        if (L.dataSize < (uint64_t)chNum * 8) throw std::runtime_error("DSF data 块过短");
        L.isDsf = true;
        L.rate = rate; L.channels = (int)chNum;
        L.bitsPerChannel = sampleCnt;
        L.bytesPerChannel = needBytes;
        L.blockSize = blockSize;
        L.paddedLastBlock = (L.dataSize >= needData);
        L.container = "dsf";
        return L;
    }
    if (ext == ".dff") {
        uint8_t h[16];
        if (rd(0, h, 16) != 16 || std::memcmp(h, "FRM8", 4) != 0)
            throw std::runtime_error("不是 DFF(DSDIFF) 文件: " + path);
        if (std::memcmp(h + 12, "DSD ", 4) != 0) {
            // form type = "DST " ⟹ 整个文件就是 DST 压缩的 DSDIFF
            if (std::memcmp(h + 12, "DST ", 4) == 0)
                throw UnsupportedFormat(msg_dst_unsupported("DFF form type=DST，文件=" + path));
            throw UnsupportedFormat("DFF 的 form type 不是 \"DSD \"（为 \"" +
                                    std::string((const char*)h + 12, 4) + "\"），暂不支持。"
                                    "请转成标准 .dff/.dsf 再播放。（文件=" + path + "）");
        }
        const uint64_t frm8Size = rdb64(h + 4);
        const uint64_t end = 12 + frm8Size;
        uint32_t rate = 0, declaredCh = 0;
        std::string cmpType;
        bool haveData = false;
        for (uint64_t off = 16; off + 12 <= end; ) {
            uint8_t ch[12];
            if (rd(off, ch, 12) != 12) break;
            const uint64_t sz = rdb64(ch + 4);
            if (sz > (1ull << 62)) break;
            if (std::memcmp(ch, "PROP", 4) == 0) {
                std::vector<uint8_t> body((size_t)sz);
                if (rd(off + 12, body.data(), body.size()) != body.size())
                    throw std::runtime_error("DFF PROP 块被截断");
                if (body.size() >= 4 && std::memcmp(body.data(), "SND ", 4) == 0) {
                    size_t q = 4;
                    while (q + 12 <= body.size()) {
                        const uint64_t ss = rdb64(body.data() + q + 4);
                        const uint8_t* p = body.data() + q + 12;
                        if (q + 12 + ss > body.size()) break;
                        if (std::memcmp(body.data() + q, "FS  ", 4) == 0 && ss >= 4) rate = rdb32(p);
                        else if (std::memcmp(body.data() + q, "CHNL", 4) == 0 && ss >= 2) declaredCh = rdb16(p);
                        else if (std::memcmp(body.data() + q, "CMPR", 4) == 0 && ss >= 4) cmpType.assign((const char*)p, 4);
                        q += 12 + (size_t)ss + (size_t)(ss & 1);
                    }
                }
            } else if (std::memcmp(ch, "DSD ", 4) == 0) {
                L.dataOffset = off + 12;
                L.dataSize = sz;
                haveData = true;
                break;
            } else if (std::memcmp(ch, "DST ", 4) == 0 || std::memcmp(ch, "DSTI", 4) == 0) {
                throw UnsupportedFormat(msg_dst_unsupported("DFF 含 DST 块（流式解码），文件=" + path));
            }
            off += 12 + sz + (sz & 1);
        }
        if (!haveData) throw std::runtime_error("DFF 缺少 DSD 数据块");
        if (rate == 0) throw std::runtime_error("DFF 缺少 FS");
        if (!cmpType.empty() && cmpType != "DSD ") {
            if (cmpType == "DST ")
                throw UnsupportedFormat(msg_dst_unsupported("DFF CMPR=DST（流式解码），文件=" + path));
            throw UnsupportedFormat("DFF 压缩类型不是 \"DSD \"（为 \"" + cmpType + "\"），暂不支持。"
                                    "请转成未压缩的 .dff/.dsf 再播放。（文件=" + path + "）");
        }
        const uint32_t ch = declaredCh ? declaredCh : 2;
        if (ch < 1 || ch > 8) throw std::runtime_error("DFF 声道数异常");
        if (L.dataSize == 0 || L.dataSize % ch != 0) throw std::runtime_error("DFF DSD 数据长度异常");
        L.isDsf = false;
        L.rate = rate; L.channels = (int)ch;
        L.bytesPerChannel = L.dataSize / ch;
        L.bitsPerChannel = L.bytesPerChannel * 8;
        L.blockSize = 0;
        L.paddedLastBlock = true;
        L.container = "dff";
        return L;
    }
    throw std::runtime_error("不是 DSD 文件（按扩展名判断）: " + path);
}


// 流式 DSD -> PCM 读取器：按「块」读原始 1-bit 流、逐块抽取，内存与文件长度无关。
// 用法：
//   DsdPcmStream s; std::string err;
//   if (!s.open(path, dsd_pcm_settings(), &err)) { /* 回退整曲路径 */ }
//   AudioBuffer chunk; s.read(0, 4096, chunk);        // 任意位置（顺序读最快）
class DsdPcmStream {
public:
    DsdPcmStream() = default;
    ~DsdPcmStream() { if (f_) std::fclose(f_); }
    DsdPcmStream(const DsdPcmStream&) = delete;
    DsdPcmStream& operator=(const DsdPcmStream&) = delete;

    bool open(const std::string& path, const DsdPcmSettings& st, std::string* errOut = nullptr) {
        close();
        try {
            layout_ = probe_dsd_layout(path);
        } catch (const std::exception& e) {
            if (errOut) *errOut = e.what();
            return false;
        }
        outRate_ = std::min<uint32_t>(layout_.rate / 16u, 705600u);
        if (outRate_ == 0 || layout_.rate % outRate_ != 0) {
            if (errOut) *errOut = "DSD 采样率无法整数抽取（" + std::to_string(layout_.rate) + " → " +
                                  std::to_string(outRate_) + "）";
            return false;
        }
        cfg_ = dsp::conversion_integrator(st.kind, st.conversion, layout_.rate, outRate_, 120.0, st.twoStage);
        dec_.clear();
        for (int c = 0; c < layout_.channels; ++c)
            dec_.push_back(std::make_unique<dsp::DsdDecimator>(cfg_));
        f_ = dsd_detail::dsd_fopen(path, L"rb", "rb");
        if (!f_) { if (errOut) *errOut = "无法打开文件: " + path; return false; }
        ch_ = layout_.channels;
        M_ = layout_.rate / outRate_;
        totalFrames_ = (size_t)(layout_.bitsPerChannel / M_);
        readBlock_ = layout_.isDsf ? layout_.blockSize : 4096u;
        if (readBlock_ == 0) readBlock_ = 4096u;
        scratch_.assign(readBlock_, 0);
        pending_.assign((size_t)ch_, {});
        reset_to(0);
        return true;
    }

    void close() {
        if (f_) { std::fclose(f_); f_ = nullptr; }
        dec_.clear();
        pending_.clear();
        curBit_ = 0; absFrames_ = 0; totalFrames_ = 0;
    }
    bool is_open() const { return f_ != nullptr; }
    uint32_t rate() const { return outRate_; }
    int channels() const { return ch_; }
    size_t totalFrames() const { return totalFrames_; }
    double seconds() const { return outRate_ ? (double)totalFrames_ / (double)outRate_ : 0.0; }
    uint64_t streamBytes() const { return layout_.dataSize; }
    const DsdFileLayout& layout() const { return layout_; }

    // 读取 [pos, pos+n) 帧，写入 out（rate/channels/bits 会被填好）。
    // 返回实际产出的帧数（到文件末尾会变短）。
    size_t read(size_t pos, size_t n, AudioBuffer& out) {
        out.rate = outRate_; out.channels = ch_; out.bits = 32; out.isFloat = false; out.raw.clear();
        if (!f_ || ch_ <= 0 || n == 0 || pos >= totalFrames_) return 0;
        if (n > totalFrames_ - pos) n = totalFrames_ - pos;
        const uint64_t wantEnd = (uint64_t)pos + (uint64_t)n;
        const uint64_t haveBegin = pendStart_;
        const uint64_t haveEnd = pendStart_ + (uint64_t)pendCount();
        // 顺序读命中 → 直接续；向后跳/回头 → 重建状态（块边界对齐）
        if ((uint64_t)pos < haveBegin || (uint64_t)pos > haveEnd) reset_to((uint64_t)pos * (uint64_t)M_);
        size_t guard = 0;
        while (pendStart_ + (uint64_t)pendCount() < wantEnd && guard++ < 1000000u) {
            if (!feed_next_block()) break;
        }
        const uint64_t availEnd = pendStart_ + (uint64_t)pendCount();
        if (availEnd <= (uint64_t)pos) return 0;
        const size_t take = (size_t)(std::min<uint64_t>(wantEnd, availEnd) - (uint64_t)pos);
        const size_t skip = (size_t)((uint64_t)pos - pendStart_);
        out.raw.resize(take * (size_t)ch_);
        for (int c = 0; c < ch_; ++c) {
            const auto& v = pending_[(size_t)c];
            for (size_t i = 0; i < take; ++i)
                out.setSample(i, c, v[skip + i]);
        }
        // 丢掉已消费的前缀，控制内存（每次最多留下不到一块）
        const size_t drop = skip + take;
        for (auto& v : pending_) v.erase(v.begin(), v.begin() + (long long)drop);
        pendStart_ = (uint64_t)pos + take;
        return take;
    }

private:
    size_t pendCount() const { return pending_.empty() ? 0 : pending_[0].size(); }

    void reset_to(uint64_t bitIndex) {
        const uint64_t blockBits = (uint64_t)readBlock_ * 8ull;
        const uint64_t aligned = (bitIndex / blockBits) * blockBits;
        curBit_ = aligned;
        absFrames_ = curBit_ / (uint64_t)M_;
        pendStart_ = absFrames_;
        for (auto& d : dec_) d->reset();
        for (auto& v : pending_) v.clear();
    }

    bool feed_next_block() {
        if (!f_) return false;
        const uint64_t blockBytes = readBlock_;
        if (curBit_ / 8ull >= layout_.bytesPerChannel) return false;
        const uint64_t b = curBit_ / (blockBytes * 8ull);
        const uint64_t byteOff = b * blockBytes;
        if (byteOff >= layout_.bytesPerChannel) return false;
        const size_t thisBlock = (size_t)std::min<uint64_t>(blockBytes, layout_.bytesPerChannel - byteOff);
        for (int c = 0; c < ch_; ++c) {
            uint64_t src = 0;
            if (layout_.isDsf) {
                const uint64_t base = layout_.dataOffset + b * (uint64_t)ch_ * (uint64_t)layout_.blockSize;
                const uint64_t stride = layout_.paddedLastBlock ? (uint64_t)layout_.blockSize : (uint64_t)thisBlock;
                src = base + (uint64_t)c * stride;
            } else {
                src = layout_.dataOffset + (uint64_t)c * layout_.bytesPerChannel + byteOff;
            }
            if (scratch_.size() < thisBlock) scratch_.resize(thisBlock, 0);
            size_t got = 0;
            if (_fseeki64(f_, (long long)src, SEEK_SET) == 0)
                got = std::fread(scratch_.data(), 1, thisBlock, f_);
            if (got < thisBlock) std::memset(scratch_.data() + got, 0, thisBlock - got);
            if (layout_.isDsf)
                for (size_t i = 0; i < thisBlock; ++i) scratch_[i] = dsd_detail::reverse_bits8(scratch_[i]);
            dec_[(size_t)c]->process_bits(scratch_.data(), thisBlock * 8, pending_[(size_t)c]);
        }
        curBit_ += (uint64_t)thisBlock * 8ull;
        absFrames_ = curBit_ / (uint64_t)M_;
        pendStart_ = absFrames_ - (uint64_t)pendCount();
        return true;
    }

    DsdFileLayout layout_;
    FILE* f_ = nullptr;
    uint32_t outRate_ = 0;
    uint32_t M_ = 1;
    int ch_ = 0;
    uint32_t readBlock_ = 4096;
    uint64_t curBit_ = 0;        // 已喂给抽取器的位数（每声道）
    size_t   absFrames_ = 0;     // 抽取器已产出的帧数
    size_t   totalFrames_ = 0;
    uint64_t pendStart_ = 0;     // pending_[c][0] 对应的绝对帧号
    dsp::IntegratorConfig cfg_;
    std::vector<std::unique_ptr<dsp::DsdDecimator>> dec_;
    std::vector<std::vector<double>> pending_;
    std::vector<uint8_t> scratch_;
};


// ---------------- 大文件流式播放策略（阈值可配置） ----------------
// 「解码后大小」超过阈值的 DSD 文件不再整曲预解码，改走 DsdPcmStream 流式解码。
// 为什么用解码后大小而不是文件大小：DSD64 的 271 MB 文件解成 PCM 是 517 MB，
// 而 DSD128/256 的文件更小、解码后更大 —— 判据必须落在真正会占内存的量上。
struct BigFileStreamSettings {
    bool    enabled = true;
    double  thresholdMb = 200.0;    // 解码后 > 200 MB 走流式（默认值来自实测：517 MB 的文件会卡 5 s）
    double  ispPrescanSec = 20.0;   // 流式播放的过采样峰值只扫开头这么长（整曲扫描要再解一遍）
};
inline BigFileStreamSettings& big_file_stream_settings() { static BigFileStreamSettings s; return s; }

// 该文件能否流式解码（只读文件头，不加载数据）。decodedBytes 返回解码后的 PCM 字节数。
inline bool dsd_streamable(const std::string& path, double* decodedMb = nullptr, std::string* why = nullptr) {
    try {
        const DsdFileLayout L = probe_dsd_layout(path);
        const uint32_t outRate = std::min<uint32_t>(L.rate / 16u, 705600u);
        if (outRate == 0 || L.rate % outRate != 0) {
            if (why) *why = "DSD 采样率无法整数抽取";
            return false;
        }
        const size_t frames = (size_t)(L.bitsPerChannel / (uint64_t)(L.rate / outRate));
        if (decodedMb) *decodedMb = (double)frames * (double)L.channels * 4.0 / (1024.0 * 1024.0);
        return true;
    } catch (const std::exception& e) {
        if (why) *why = e.what();
        return false;
    }
}

// 是否应该对这次播放走流式（只看文件头）：DSD 且解码后超过阈值 且 开关打开。
inline bool should_stream_decode(const std::string& path, double* decodedMb = nullptr, std::string* why = nullptr) {
    const std::string ext = lowerExt(path);
    if (ext != ".dsf" && ext != ".dff") { if (why) *why = "非 DSD 文件"; return false; }
    const BigFileStreamSettings& s = big_file_stream_settings();
    if (!s.enabled) { if (why) *why = "流式播放已关闭(stream_threshold_mb=0)"; return false; }
    double mb = 0.0;
    std::string whyLocal;
    if (!dsd_streamable(path, &mb, &whyLocal)) { if (why) *why = whyLocal; return false; }
    if (decodedMb) *decodedMb = mb;
    if (mb <= s.thresholdMb) { if (why) *why = "解码后 " + std::to_string((long long)mb) + " MB 低于阈值"; return false; }
    return true;
}

// DSD 源：原生读取 + 我们的抽取器（ROADMAP P0-1/P0-4/P0-5）
//
// 以前 .dsf/.dff 走 ffmpeg 解成 PCM：多一层量化、拿不到 1-bit 流、DirectSDM 无从谈起。
// 现在读原始位流，按设置用 FIR / IIR / CIC 抽取到 DSD/16（上限 705.6 kHz），
// 后续升频/抖动/SDM 完全不变。读取失败仍回退 ffmpeg（保持可用）。
inline AudioBuffer read_dsd_native(const std::string& path, LoadNote* note) {
    const auto t_dsd_0 = std::chrono::steady_clock::now();
    DsdStream s = read_dsd(path);
    const uint32_t outRate = std::min<uint32_t>(s.rate / 16u, 705600u);
    const DsdPcmSettings st = dsd_pcm_settings();
    const dsp::IntegratorConfig cfg =
        dsp::conversion_integrator(st.kind, st.conversion, s.rate, outRate, 120.0, st.twoStage);
    dsp::DsdDecimator dec(cfg);
    auto planes = dec.process(s);

    AudioBuffer out;
    out.rate = outRate;
    out.channels = s.channels;
    out.bits = 32;
    out.isFloat = false;
    const size_t n = planes.empty() ? 0 : planes[0].size();
    out.raw.resize(n * (size_t)s.channels);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < s.channels; ++c)
            out.setSample(i, c, planes[(size_t)c][i]);

    const auto t_dsd_1 = std::chrono::steady_clock::now();
    if (note) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "DSD 原生解码：%s %u Hz → PCM %u Hz（%s%s / %s 通带 %.1f kHz / 阻带 %.1f kHz）",
                      s.container.c_str(), s.rate, outRate,
                      dec.isTwoStage() ? "两级 " : "",
                      dsp::integrator_name(cfg.kind), dsp::conversion_name(st.conversion),
                      cfg.passbandHz / 1000.0, dec.stopbandStartHz() / 1000.0);
        note->text = buf;
        note->totalMs = std::chrono::duration<double, std::milli>(t_dsd_1 - t_dsd_0).count();
    }
    return out;
}

// ffmpeg 解码：写临时 WAV 文件再原生读取（旧路径，作为流式不支持或关闭时的完整回退路径）
inline AudioBuffer read_via_ffmpeg_file(const std::string& path, LoadNote* note = nullptr) {
    const std::string ext = lowerExt(path);
    const std::string ffmpeg  = find_tool("ffmpeg");
    const std::string ffprobe = find_tool("ffprobe");
    if (ffmpeg.empty()) {
        throw std::runtime_error(
            "需要 ffmpeg 才能解码 " + ext + " 文件：把 ffmpeg.exe（以及 ffprobe.exe）放到播放器"
            "同目录，或加进 PATH。WAV / DSF / DFF 不需要 ffmpeg。");
    }

    // 2026-10-27（D7）：浮点解码器走 pcm_f32le，整数解码器仍走对应整数位深（见 probe_ffmpeg_pcm）
    const FfmpegPcmChoice pcm = probe_ffmpeg_pcm(ffprobe, path);
    const char* codec = pcm.codec;

    wchar_t tmpDir[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, tmpDir) == 0)
        throw std::runtime_error("取临时目录失败（GetTempPathW 返回 0），无法解码 " + path);
    wchar_t tmpName[MAX_PATH] = {0};
    // GetTempFileNameW 返回 0 = 失败，此时 tmpName 内容未定义（绝不能拿去拼命令行）。
    // uUnique=0 表示由系统原子地选一个未占用的名字 —— 这也是并发解码不会互相抢文件名的依据。
    if (GetTempFileNameW(tmpDir, L"rta", 0, tmpName) == 0)
        throw std::runtime_error("创建临时文件失败（GetTempFileNameW 返回 0，临时目录=" +
                                 wide_to_utf8(tmpDir) + "，可能是磁盘满或 %TEMP% 不可写）");
    std::string tmpPath;
    {
        const int m = WideCharToMultiByte(CP_UTF8, 0, tmpName, -1, nullptr, 0, nullptr, nullptr);
        tmpPath.resize((size_t)m);
        WideCharToMultiByte(CP_UTF8, 0, tmpName, -1, tmpPath.data(), m, nullptr, nullptr);
        if (!tmpPath.empty() && tmpPath.back() == '\0') tmpPath.pop_back();
    }

    const std::wstring cmd = L"\"" + widen(ffmpeg) + L"\" -v error -y -i \"" + widen(path) +
                             L"\" -vn -c:a " + widen(codec) + L" -f wav \"" + widen(tmpPath) + L"\"";

    // ---- 瞬时故障重试（2026-10-22 现场 BUG：FLAC 间歇性 -42 Illegal byte sequence）----
    // 根因有两层，都在这里治：
    //   ① ffmpeg 打不开输入时会【回退到 ANSI 代码页】再试一次；含非 ASCII 字符的路径在这个
    //      回退里被 MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS) 判成非法序列，于是
    //      「文件此刻不在」/「被独占占用」统统被报成 EILSEQ(-42 "Illegal byte sequence")。
    //      实测（包内 ffmpeg 9.0）：日文名 + 存在 → exit=0；日文名 + 独占占用 → exit=-42；
    //      纯 ASCII 名 + 独占占用 → exit=-13(Permission denied)。即 -42 是【掩码】不是原因。
    //   ② 触发这类瞬时不可读的现实场景：标签写回(备份 rename + 替换 rename 的窗口)、
    //      同步客户端/杀毒扫描短暂占用、用户在资源管理器里改名。
    // 所以：失败后用 Win32 问「此刻到底能不能读、为什么读不了」。
    // 注意：首次失败（i == 0）后必须至少再试一次，「此刻可读」不能排除「刚刚在 ffmpeg 运行期间才释放独占」；
    // 只有在已重试（i >= 1）且探针证明此刻仍可读时，才说明是真正的解码错误并提前退出。
    const auto t_total_0 = std::chrono::steady_clock::now();
    constexpr int kDecodeTries = 3;
    int rc = -1;
    int attempts = 0;
    std::string fflog;                     // ffmpeg 的 stderr（错误时进错误文本，不吞不瞒）
    std::string unreadable;                // 非空 = 此刻源文件读不了（Win32 权威原因）
    double ffmpeg_ms = 0.0;
    for (int i = 0; i < kDecodeTries; ++i) {
        ++attempts;
        fflog.clear();
        if (i > 0) {
            // 上一轮可能留下半成品；清掉再让 ffmpeg 重建，避免把半截 WAV 当产物
            DeleteFileW(widen(tmpPath).c_str());
            Sleep(120u * (DWORD)i);        // 退避 120ms / 240ms，等占用方松手
        }
        const auto t_ff_0 = std::chrono::steady_clock::now();
        rc = run_hidden(cmd, &fflog);
        const auto t_ff_1 = std::chrono::steady_clock::now();
        ffmpeg_ms += std::chrono::duration<double, std::milli>(t_ff_1 - t_ff_0).count();
        if (rc == 0) break;
        unreadable = source_read_probe(path);
        // 关键判据修正（2026-10-23 回归）：除 rc==0 退出外，首轮失败（i == 0）必须至少再试一次。
        // 根因：在真实场景（标签写回 rename 窗口、杀毒扫描或同步客户端短暂独占）以及 CI 云机环境中，
        // 占用方极可能恰好在 ffmpeg 尝试期间松手；ffmpeg 进程退出时探针虽然检测到「此刻可读」
        // （unreadable 为空），但这恰恰证明瞬时占用刚刚解除，进行第二轮重试即可成功！
        // 若在 i==0 时因「此刻可读」就 break，就会把本可恢复的瞬时占用直接报成解码失败（现场那种
        // 「重试一下就好」的用户痛点，以及 CI 云机上 ffmpeg 耗时 >150ms 导致尝试 1 次即失败的竞态）。
        // 因此：只有在已经至少重试过一次之后（i >= 1），且探针明确说明源文件此刻可读时，
        // 才能证明故障并非瞬时占用引起（而是真正的格式/解码损坏），此时方可提前 break 退出。
        if (i >= 1 && unreadable.empty()) break;
    }

    // ffmpeg 输出压成单行、截断，方便进一行的错误日志
    std::string ffTail;
    for (char c : fflog) {
        if (c == '\r' || c == '\n') { if (!ffTail.empty() && ffTail.back() != ' ') ffTail.push_back(' '); }
        else ffTail.push_back(c);
    }
    if (!ffTail.empty() && ffTail.back() == ' ') ffTail.pop_back();
    if (ffTail.size() > 300) ffTail = ffTail.substr(0, 300) + "...";

    // HF-03：ffmpeg 退出后先检查临时 WAV 存在与大小，再交给 read_wav。
    // 产物缺失/过小说明 ffmpeg 解码异常，直接报「ffmpeg 解码产物异常」并带路径与字节数。
    auto size_of = [](const std::wstring& p) -> long long {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return -1;
        return ((long long)fa.nFileSizeHigh << 32) | (long long)fa.nFileSizeLow;
    };
    const long long tmpBytes = size_of(widen(tmpPath));
    const std::string tmpBytesTxt = (tmpBytes < 0) ? "未知(文件不存在)" : (std::to_string(tmpBytes) + " 字节");

    AudioBuffer buf;
    double wav_ms = 0.0;
    try {
        if (rc != 0) {
            // -42 = -EILSEQ。ffmpeg 对非 ASCII 路径会用它掩盖真实原因（详见上面重试循环的注释），
            // 所以这里把 Win32 探到的真实原因放在最前面：日志读起来必须是「读不了，因为 X」，
            // 而不是让用户/我们自己去猜 "Illegal byte sequence" 是什么意思。
            std::string why;
            if (rc == -42) {
                why = "ffmpeg 报非法字节序列(-42/EILSEQ)。该退出码只说明 ffmpeg 在非 ASCII 路径上"
                      "回退到 ANSI 代码页后失败，不代表文件真有编码问题";
                why += unreadable.empty()
                    ? "（重试期间源文件已可读：更像是 ffmpeg 打开瞬间的偶发失败）"
                    : "；真实原因：" + unreadable;
            } else {
                why = "ffmpeg 解码失败";
                if (!unreadable.empty()) why += "（源文件此刻读不了：" + unreadable + "）";
            }
            throw std::runtime_error(why + "（退出码 " + std::to_string(rc) +
                                     "，尝试 " + std::to_string(attempts) + " 次，源文件=" + path +
                                     "，临时WAV=" + tmpPath + "，大小=" + tmpBytesTxt +
                                     (ffTail.empty() ? "" : "，ffmpeg输出=" + ffTail) + "）");
        }
        if (tmpBytes < 44)
            throw std::runtime_error("ffmpeg 解码产物异常: 临时 WAV 缺失或过小(源文件=" + path +
                                     "，临时WAV=" + tmpPath + "，大小=" + tmpBytesTxt + "，退出码=0)");
        const auto t_wav_0 = std::chrono::steady_clock::now();
        buf = read_wav(tmpPath);
        const auto t_wav_1 = std::chrono::steady_clock::now();
        wav_ms = std::chrono::duration<double, std::milli>(t_wav_1 - t_wav_0).count();
    } catch (...) {
        DeleteFileW(widen(tmpPath).c_str());
        throw;
    }
    DeleteFileW(widen(tmpPath).c_str());

    const auto t_total_1 = std::chrono::steady_clock::now();
    if (note) {
        note->text = std::string("已用 ffmpeg 解码 ") + ext + " -> " + codec;
        note->ffmpegMs = ffmpeg_ms;
        note->wavReadMs = wav_ms;
        note->totalMs = std::chrono::duration<double, std::milli>(t_total_1 - t_total_0).count();
    }
    return buf;
}

// ffmpeg 流式解码路径（ROADMAP P6b：直接读管道输出，不落临时 WAV 文件）
inline AudioBuffer read_via_ffmpeg_stream(const std::string& path, LoadNote* note = nullptr) {
    const std::string ext = lowerExt(path);
    const std::string ffmpeg  = find_tool("ffmpeg");
    const std::string ffprobe = find_tool("ffprobe");
    if (ffmpeg.empty()) {
        throw std::runtime_error(
            "需要 ffmpeg 才能解码 " + ext + " 文件：把 ffmpeg.exe（以及 ffprobe.exe）放到播放器"
            "同目录，或加进 PATH。WAV / DSF / DFF 不需要 ffmpeg。");
    }

    // 2026-10-27（D7）：浮点解码器走 pcm_f32le，整数解码器仍走对应整数位深（见 probe_ffmpeg_pcm）
    const FfmpegPcmChoice pcm = probe_ffmpeg_pcm(ffprobe, path);
    const char* codec = pcm.codec;
    const std::wstring cmd = L"\"" + widen(ffmpeg) + L"\" -v error -i \"" + widen(path) +
                             L"\" -vn -c:a " + widen(codec) + L" -f wav pipe:1";
    const auto t_total_0 = std::chrono::steady_clock::now();
    constexpr int kDecodeTries = 3;
    int rc = -1;
    int attempts = 0;
    std::string fflog;
    std::string unreadable;
    double ffmpeg_ms = 0.0;
    AudioBuffer buf;
    bool success = false;
    std::string streamError;

    for (int i = 0; i < kDecodeTries; ++i) {
        ++attempts;
        fflog.clear();
        streamError.clear();
        buf = AudioBuffer{};

        if (i > 0) {
            Sleep(120u * (DWORD)i); // 退避 120ms / 240ms，等占用方松手
        }

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE outRd = nullptr, outWr = nullptr;
        HANDLE errRd = nullptr, errWr = nullptr;
        if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
            rc = -1;
            streamError = "CreatePipe(stdout) 失败";
            unreadable = source_read_probe(path);
            if (i >= 1 && unreadable.empty()) break;
            continue;
        }
        if (!CreatePipe(&errRd, &errWr, &sa, 0)) {
            CloseHandle(outRd); CloseHandle(outWr);
            rc = -1;
            streamError = "CreatePipe(stderr) 失败";
            unreadable = source_read_probe(path);
            if (i >= 1 && unreadable.empty()) break;
            continue;
        }
        SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = outWr;
        si.hStdError  = errWr;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::wstring tmpCmd = cmd;
        const auto t_ff_0 = std::chrono::steady_clock::now();
        BOOL cp = CreateProcessW(nullptr, tmpCmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (!cp) {
            CloseHandle(outRd); CloseHandle(outWr);
            CloseHandle(errRd); CloseHandle(errWr);
            rc = -1;
            streamError = "CreateProcessW 启动 ffmpeg 失败";
            unreadable = source_read_probe(path);
            if (i >= 1 && unreadable.empty()) break;
            continue;
        }

        // 立即关闭写端句柄，使得子进程写完后读端能正常收到 EOF
        CloseHandle(outWr);
        CloseHandle(errWr);

        // 启动后台线程读取 stderr，避免 stderr 管道被填满导致子进程阻塞挂起
        std::thread errThread([errRd, &fflog] {
            char ebuf[4096];
            DWORD eg = 0;
            while (ReadFile(errRd, ebuf, sizeof(ebuf), &eg, nullptr) && eg > 0) {
                fflog.append(ebuf, eg);
            }
            CloseHandle(errRd);
        });

        // 主线程读取 stdout 管道流并解析填充 AudioBuffer
        bool readOk = false;
        try {
            readOk = read_wav_stream_pipe(outRd, pi.hProcess, buf, &streamError);
        } catch (const std::exception& ex) {
            streamError = ex.what();
            readOk = false;
        }
        CloseHandle(outRd);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        errThread.join();

        const auto t_ff_1 = std::chrono::steady_clock::now();
        ffmpeg_ms += std::chrono::duration<double, std::milli>(t_ff_1 - t_ff_0).count();
        rc = (int)exitCode;

        if (rc == 0 && readOk && buf.frames() > 0) {
            success = true;
            break;
        }

        unreadable = source_read_probe(path);
        // 关键判据修正（2026-10-23 回归）：首轮失败（i == 0）必须至少再试一次
        if (i >= 1 && unreadable.empty()) break;
    }

    std::string ffTail;
    for (char c : fflog) {
        if (c == '\r' || c == '\n') { if (!ffTail.empty() && ffTail.back() != ' ') ffTail.push_back(' '); }
        else ffTail.push_back(c);
    }
    if (!ffTail.empty() && ffTail.back() == ' ') ffTail.pop_back();
    if (ffTail.size() > 300) ffTail = ffTail.substr(0, 300) + "...";

    if (!success) {
        std::string why;
        if (rc == -42) {
            why = "ffmpeg 报非法字节序列(-42/EILSEQ)。该退出码只说明 ffmpeg 在非 ASCII 路径上"
                  "回退到 ANSI 代码页后失败，不代表文件真有编码问题";
            why += unreadable.empty()
                ? "（重试期间源文件已可读：更像是 ffmpeg 打开瞬间的偶发失败）"
                : "；真实原因：" + unreadable;
        } else if (rc != 0) {
            why = "ffmpeg 解码失败";
            if (!unreadable.empty()) why += "（源文件此刻读不了：" + unreadable + "）";
        } else {
            why = "ffmpeg 解码产物异常: 流式解析失败(" + (streamError.empty() ? "帧数为空" : streamError) + ")";
        }
        throw std::runtime_error(why + "（退出码 " + std::to_string(rc) +
                                 "，尝试 " + std::to_string(attempts) + " 次，源文件=" + path +
                                 (ffTail.empty() ? "" : "，ffmpeg输出=" + ffTail) + "）");
    }

    const auto t_total_1 = std::chrono::steady_clock::now();
    if (note) {
        note->text = std::string("已用 ffmpeg 流式解码 ") + ext + " -> " + codec;
        note->ffmpegMs = ffmpeg_ms;
        note->wavReadMs = 0.0;
        note->totalMs = std::chrono::duration<double, std::milli>(t_total_1 - t_total_0).count();
    }
    return buf;
}

// ffmpeg 解码统一入口（非 WAV / 非 DSD，或 DSD 原生读取失败时的回退路径）
// 优先走流式管道读取（免落临时 WAV 文件）；对需要 seek 的特殊容器或设置关闭时自动回退临时文件。
inline AudioBuffer read_via_ffmpeg(const std::string& path, LoadNote* note = nullptr) {
    if (!is_stream_decode_enabled()) {
        return read_via_ffmpeg_file(path, note);
    }
    try {
        return read_via_ffmpeg_stream(path, note);
    } catch (const std::exception& e) {
        // 源文件探针归因：如果源文件此刻确实读不了（独占占用、文件不存在、无权限），
        // 回退临时文件同样无法解决，必须直接重抛保留权威的 Win32 归因与原始退出码。
        const std::string unreadable = source_read_probe(path);
        if (!unreadable.empty()) {
            throw;
        }
        // 源文件此刻可读，说明流式解码失败是由于流式管道/容器特性（如需 seek 的特殊容器或管道解析失败），
        // 自动回退到既有临时 WAV 文件路径，并在 note 里详细说明回退原因。
        LoadNote fileNote;
        AudioBuffer b = read_via_ffmpeg_file(path, &fileNote);
        if (note) {
            note->text = "流式解码不适用(" + std::string(e.what()) + ")，已回退临时文件解码：" + fileNote.text;
            note->ffmpegMs = fileNote.ffmpegMs;
            note->wavReadMs = fileNote.wavReadMs;
            note->totalMs = fileNote.totalMs;
        }
        return b;
    }
}

// 统一读取：WAV 原生，DSD 原生（失败回退 ffmpeg），其他格式经 ffmpeg
// HF-03：外层把巨量分配类异常（std::length_error="vector too long" / std::bad_alloc）
// 转成带文件与阶段的明确错误 —— 捕获只为带上下文重抛，绝不吞掉后继续（交接单 §6 红线）。
inline AudioBuffer read_any(const std::string& path, LoadNote* note = nullptr) {
    const std::string ext = lowerExt(path);
    const auto t_any_0 = std::chrono::steady_clock::now();
    // .iso 在**最前面**拦（2026-10-27，261027-01 #3）：ISO 镜像不是 ffmpeg 能碰的容器
    // （随包 ffmpeg 9.0 没有 sacd/iso9660/udf demuxer），落到下面只会得到
    // "Invalid data found when processing input" —— 用户会以为文件坏了。
    // 这里直接给「SACD 需转换 / 这不是音频镜像」的指引。
    if (ext == ".iso") throw_for_iso(path);
    try {
        if (ext == ".wav" || ext == ".wave" || ext.empty()) {
            AudioBuffer b = read_wav(path);
            if (note) {
                const auto t_any_1 = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(t_any_1 - t_any_0).count();
                note->wavReadMs = ms;
                note->totalMs = ms;
            }
            return b;
        }
        if (ext == ".dsf" || ext == ".dff") {
            try {
                AudioBuffer b = read_dsd_native(path, note);
                if (note && note->totalMs <= 0.0) {
                    const auto t_any_1 = std::chrono::steady_clock::now();
                    note->totalMs = std::chrono::duration<double, std::milli>(t_any_1 - t_any_0).count();
                }
                return b;
            } catch (const std::length_error& e) {
                throw std::runtime_error(std::string("解码阶段异常: ") + e.what() +
                                         "(文件=" + path + ",阶段=read_dsd_native)");
            } catch (const std::bad_alloc& e) {
                throw std::runtime_error(std::string("解码阶段异常: ") + e.what() +
                                         "(文件=" + path + ",阶段=read_dsd_native)");
            } catch (const UnsupportedFormat&) {
                // ★ 不支持 ≠ 读不了（2026-10-27，261027-01 #3）：DST 压缩 / 非标准位深这类
                //   必须原样上抛带指引的文案，**绝不回退 ffmpeg** —— 回退只会再失败一次，
                //   然后把 "Invalid data found when processing input" 当成结论交给用户。
                throw;
            } catch (const std::exception& e) {
                // 回退策略（ROADMAP P0）：读不了的文件仍走 ffmpeg，并在 note 里说明。
                // 注意 length_error/bad_alloc 已在上面被拦截重抛 —— 不允许"vector too long"
                // 被这个回退吞掉后继续播放。
                LoadNote tmp;
                AudioBuffer b = read_via_ffmpeg(path, &tmp);
                if (note) {
                    note->text = std::string("DSD 原生读取失败（") + e.what() + "），已回退 ffmpeg：" + tmp.text;
                    note->ffmpegMs = tmp.ffmpegMs;
                    note->wavReadMs = tmp.wavReadMs;
                    note->totalMs = tmp.totalMs;
                }
                return b;
            }
        }
        return read_via_ffmpeg(path, note);
    } catch (const std::length_error& e) {
        throw std::runtime_error(std::string("解码阶段异常: ") + e.what() +
                                 "(文件=" + path + ",阶段=read_any)");
    } catch (const std::bad_alloc& e) {
        throw std::runtime_error(std::string("解码阶段异常: ") + e.what() +
                                 "(文件=" + path + ",阶段=read_any)");
    } catch (const UnsupportedFormat&) {
        throw;                                  // 已经是人话，不再包一层
    } catch (const std::exception& e) {
        // ffmpeg 的兜底文案对用户毫无意义（2026-10-27，261027-01 #3）：
        // "Invalid data found when processing input" 的真实含义是「容器/编码 ffmpeg 不认识」，
        // 用户读成「我的文件坏了」。这里只在出现这句时补上**可能的原因 + 下一步**，
        // 其余异常原样上抛（不吞、不改类型，调用方与测试看到的仍是同一条错误）。
        const std::string m = e.what();
        if (m.find("Invalid data") != std::string::npos) {
            throw UnsupportedFormat(
                "这个文件 ffmpeg 解不了（它报的原文是 \"Invalid data found when processing input\"，"
                "含义是容器/编码不可识别，不等于文件损坏）。常见原因是：DST 压缩的 DSD（需先转 .dsf/.dff）、"
                "SACD ISO 镜像（需先 sacd_extract 分轨）、加密或下载不完整的文件。"
                "（文件=" + path + "，阶段=read_any，ffmpeg 原始报错=" + m + "）");
        }
        throw;
    }
}

} // namespace rta::audio
