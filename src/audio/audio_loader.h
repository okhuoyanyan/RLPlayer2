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
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#include <new>

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

struct LoadNote {
    std::string text;
};

// DSD 源抽取设置（由 rtaweb / rtaplay 按配置写入；默认 FIR + narrow）
// 为什么用全局：read_any 的签名在很多地方被调用，加参数会波及媒体信息、库扫描与预解码槽。
// 代价是"播放线程与预解码线程可能短暂看到不同设置"，最多影响一首歌的开头，可接受。
struct DsdPcmSettings {
    dsp::IntegratorKind kind = dsp::IntegratorKind::Fir;
    dsp::DsdConversion  conversion = dsp::DsdConversion::Narrow;
};
inline DsdPcmSettings& dsd_pcm_settings() { static DsdPcmSettings s; return s; }

// DSD 源：原生读取 + 我们的抽取器（ROADMAP P0-1/P0-4/P0-5）
//
// 以前 .dsf/.dff 走 ffmpeg 解成 PCM：多一层量化、拿不到 1-bit 流、DirectSDM 无从谈起。
// 现在读原始位流，按设置用 FIR / IIR / CIC 抽取到 DSD/16（上限 705.6 kHz），
// 后续升频/抖动/SDM 完全不变。读取失败仍回退 ffmpeg（保持可用）。
inline AudioBuffer read_dsd_native(const std::string& path, LoadNote* note) {
    DsdStream s = read_dsd(path);
    const uint32_t outRate = std::min<uint32_t>(s.rate / 16u, 705600u);
    const DsdPcmSettings st = dsd_pcm_settings();
    const dsp::IntegratorConfig cfg =
        dsp::conversion_integrator(st.kind, st.conversion, s.rate, outRate, 120.0);
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

    if (note) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "DSD 原生解码：%s %u Hz → PCM %u Hz（%s / %s 通带 %.1f kHz / 阻带 %.1f kHz）",
                      s.container.c_str(), s.rate, outRate,
                      dsp::integrator_name(cfg.kind), dsp::conversion_name(st.conversion),
                      cfg.passbandHz / 1000.0, dec.stopbandStartHz() / 1000.0);
        note->text = buf;
    }
    return out;
}

// ffmpeg 解码（非 WAV / 非 DSD，或 DSD 原生读取失败时的回退路径）
inline AudioBuffer read_via_ffmpeg(const std::string& path, LoadNote* note) {
    const std::string ext = lowerExt(path);
    const std::string ffmpeg  = find_tool("ffmpeg");
    const std::string ffprobe = find_tool("ffprobe");
    if (ffmpeg.empty()) {
        throw std::runtime_error(
            "需要 ffmpeg 才能解码 " + ext + " 文件：把 ffmpeg.exe（以及 ffprobe.exe）放到播放器"
            "同目录，或加进 PATH。WAV / DSF / DFF 不需要 ffmpeg。");
    }

    const int bits = probe_bit_depth(ffprobe, path);
    const char* codec = (bits <= 16) ? "pcm_s16le" : (bits <= 24 ? "pcm_s24le" : "pcm_s32le");

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
    // 所以：失败后先用 Win32 自己问一次「此刻到底能不能读、为什么读不了」，再决定重试。
    // 只要源文件此刻读得了，重试没有意义（那是真解码错误），立刻退出重试循环。
    constexpr int kDecodeTries = 3;
    int rc = -1;
    int attempts = 0;
    std::string fflog;                     // ffmpeg 的 stderr（错误时进错误文本，不吞不瞒）
    std::string unreadable;                // 非空 = 此刻源文件读不了（Win32 权威原因）
    for (int i = 0; i < kDecodeTries; ++i) {
        ++attempts;
        fflog.clear();
        if (i > 0) {
            // 上一轮可能留下半成品；清掉再让 ffmpeg 重建，避免把半截 WAV 当产物
            DeleteFileW(widen(tmpPath).c_str());
            Sleep(120u * (DWORD)i);        // 退避 120ms / 240ms，等占用方松手
        }
        rc = run_hidden(cmd, &fflog);
        if (rc == 0) break;
        unreadable = source_read_probe(path);
        if (unreadable.empty()) break;     // 源文件读得了 ⟹ 不是瞬时占用/缺失，重试无益
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
        buf = read_wav(tmpPath);
    } catch (...) {
        DeleteFileW(widen(tmpPath).c_str());
        throw;
    }
    DeleteFileW(widen(tmpPath).c_str());

    if (note) note->text = std::string("已用 ffmpeg 解码 ") + ext + " -> " + codec;
    return buf;
}

// 统一读取：WAV 原生，DSD 原生（失败回退 ffmpeg），其他格式经 ffmpeg
// HF-03：外层把巨量分配类异常（std::length_error="vector too long" / std::bad_alloc）
// 转成带文件与阶段的明确错误 —— 捕获只为带上下文重抛，绝不吞掉后继续（交接单 §6 红线）。
inline AudioBuffer read_any(const std::string& path, LoadNote* note = nullptr) {
    const std::string ext = lowerExt(path);
    try {
        if (ext == ".wav" || ext == ".wave" || ext.empty()) {
            return read_wav(path);
        }
        if (ext == ".dsf" || ext == ".dff") {
            try {
                return read_dsd_native(path, note);
            } catch (const std::length_error& e) {
                throw std::runtime_error(std::string("解码阶段异常: ") + e.what() +
                                         "(文件=" + path + ",阶段=read_dsd_native)");
            } catch (const std::bad_alloc& e) {
                throw std::runtime_error(std::string("解码阶段异常: ") + e.what() +
                                         "(文件=" + path + ",阶段=read_dsd_native)");
            } catch (const std::exception& e) {
                // 回退策略（ROADMAP P0）：读不了的文件仍走 ffmpeg，并在 note 里说明。
                // 注意 length_error/bad_alloc 已在上面被拦截重抛 —— 不允许"vector too long"
                // 被这个回退吞掉后继续播放。
                LoadNote tmp;
                AudioBuffer b = read_via_ffmpeg(path, &tmp);
                if (note) note->text = std::string("DSD 原生读取失败（") + e.what() + "），已回退 ffmpeg：" + tmp.text;
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
    }
}

} // namespace rta::audio
