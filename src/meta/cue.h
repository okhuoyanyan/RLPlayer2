// src/meta/cue.h — CUE 整轨分轨解析（2026-10-25，第一里程碑）
//
// 背景：docs/CUE-ISO-SACD支持现状.md 的核查结论是「仓库里完全没有 CUE 解析」，
// 发烧友常见的「整轨 FLAC/WAV + .cue」只能整张从头播。这里补上【解析】这一层：
//   * 只做解析：把 .cue 变成带时间偏移的轨道列表，不打开音频、不碰播放、不碰设备。
//   * 偏移以 **1/75 秒帧**（CD 帧）的整数保存，再按音频采样率换算成采样帧 ——
//     全程整数，不引入浮点漂移。播放侧要接偏移就直接吃 cue_frames_to_samples() 的结果。
//   * 编码：先按 UTF-8（含 BOM）读，失败回退 GBK（CP936）。中文 cue 绝大多数是 GBK，
//     这条回退是必需项而不是锦上添花（tests/test_cue.cpp 有 GBK 用例逐字段比对）。
//
// CUE 规范要点（按规范实现，不猜方言）：
//   FILE "name" TYPE         TYPE 常见 WAVE / MP3 / BINARY（数据轨）
//   TRACK nn TYPE            nn 两位；TYPE = AUDIO / MODE1/2352 / MODE2/2352 …
//   INDEX 00|01 mm:ss:ff     ff 是 1/75 秒；**INDEX 01 才是曲目起点**，
//                            INDEX 00 是前导间隙（pregap）的起点，只记录、不参与偏移
//   PREGAP / POSTGAP mm:ss:ff 只记录（展开时**不改**偏移，否则会和 INDEX 01 重复扣一遍）
//   TITLE / PERFORMER        全局（专辑级）与 track 级都合法
//   REM GENRE / REM DATE     全局元数据（部分 cue 写 REM YEAR，一并接受）
//   CATALOG / ISRC           专辑目录号 / 轨 ISRC
//   多 FILE：一条 cue 里可以有多段 FILE，每段内 TRACK/INDEX 从 0 重新计 ——
//            所以每条轨道都自带「我属于哪个文件」（CueTrack::file）。
//
// ⚠️ 未解决项（铁律 4）：
//   1) 编码判定是启发式：GBK 文本若恰好也是合法 UTF-8（极少见）会被当 UTF-8 解；
//      没有做「两种解码都试 + 打分」的二次裁决。
//   2) 非 Windows 平台没有 CP936 转换表，cue_gbk_to_utf8() 原样返回字节（本仓库只构建 Windows）。
//   3) 方言指令（CDTEXTFILE / FLAGS / SONGWRITER 之外的自造指令）只记 warning，不解释含义。
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace rta::meta {

namespace fsc = std::filesystem;

// ---------------------------------------------------------------------------
// 一、编码：UTF-8（含 BOM）优先，失败回退 GBK
// ---------------------------------------------------------------------------

// 严格 UTF-8 校验：拒绝超长编码（overlong）、UTF-16 代理区（U+D800..DFFF）、> U+10FFFF。
// 为什么要严格：GBK 中文的双字节序列里大量出现 0x80-0xFF 的「像 UTF-8 前导字节」，
// 宽松校验（只看首字节）会把 GBK 误判成 UTF-8，于是中文全变问号。
inline bool cue_utf8_valid(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        int len = 0;
        unsigned int cp = 0;
        if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
        else return false;                                  // 0x80-0xBF 独立出现 / 0xF8+ 非法
        if (i + (size_t)len > n) return false;
        for (int k = 1; k < len; ++k) {
            if ((p[i + (size_t)k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + (size_t)k] & 0x3Fu);
        }
        if ((len == 2 && cp < 0x80u) || (len == 3 && cp < 0x800u) || (len == 4 && cp < 0x10000u))
            return false;                                   // overlong
        if (cp >= 0xD800u && cp <= 0xDFFFu) return false;    // 代理区
        if (cp > 0x10FFFFu) return false;
        i += (size_t)len;
    }
    return true;
}

inline std::string cue_from_wstring(const std::wstring& w) {
#if defined(_WIN32)
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}

inline std::wstring cue_widen_utf8(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

// UTF-8 路径 -> std::filesystem::path（Windows 上必须走宽字符，否则非 ASCII 目录名打不开）
inline fsc::path cue_u8path(const std::string& s) {
#if defined(_WIN32)
    return fsc::path(cue_widen_utf8(s));
#else
    return fsc::path(s);
#endif
}

inline std::string cue_path_utf8(const fsc::path& p) {
#if defined(_WIN32)
    return cue_from_wstring(p.wstring());
#else
    return p.string();
#endif
}

// UTF-16（记事本「Unicode」/「Unicode big endian」另存的 cue 不少见）→ UTF-8
inline std::string cue_utf16_to_utf8(const std::string& raw, bool bigEndian) {
    std::wstring w;
    w.reserve((raw.size() - 2) / 2 + 1);
    for (size_t i = 2; i + 1 < raw.size(); i += 2) {
        const unsigned char a = (unsigned char)raw[i], b = (unsigned char)raw[i + 1];
        const unsigned int u = bigEndian ? ((unsigned int)a << 8 | b) : ((unsigned int)b << 8 | a);
        w.push_back((wchar_t)u);
    }
    return cue_from_wstring(w);
}

// GBK（CP936）→ UTF-8。先严格解（非法序列会失败），再宽松解（坏字节变 U+FFFD，不丢上下文）。
inline std::string cue_gbk_to_utf8(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) return std::string();
    int n = ::MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) n = ::MultiByteToWideChar(936, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return s;                       // 连宽松解码都失败：原样返回，绝不丢字节
    std::wstring w((size_t)n, L'\0');
    if (::MultiByteToWideChar(936, 0, s.c_str(), (int)s.size(), w.data(), n) <= 0) return s;
    return cue_from_wstring(w);
#else
    return s;                                   // 非 Windows：没有 CP936 表，原样返回（见文件头未解决项 2）
#endif
}

// 整份 .cue 文本的编码判定：UTF-16 BOM → UTF-8 BOM → 严格 UTF-8 → GBK
inline std::string cue_decode_text(const std::string& raw) {
    if (raw.size() >= 2) {
        const unsigned char b0 = (unsigned char)raw[0], b1 = (unsigned char)raw[1];
        if (b0 == 0xFF && b1 == 0xFE) return cue_utf16_to_utf8(raw, false);
        if (b0 == 0xFE && b1 == 0xFF) return cue_utf16_to_utf8(raw, true);
    }
    std::string body = raw;
    bool hadBom = false;
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF &&
        (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF) {
        body = body.substr(3);
        hadBom = true;
    }
    if (body.empty()) return std::string();
    // 无 BOM 的合法 UTF-8 直接采信；有 BOM 但内容不是 UTF-8（罕见脏文件）也走 GBK 回退
    if (cue_utf8_valid(body)) return body;
    (void)hadBom;
    return cue_gbk_to_utf8(body);
}

// ---------------------------------------------------------------------------
// 二、时间：mm:ss:ff（ff = 1/75 秒）→ 1/75 秒帧 → 采样帧
// ---------------------------------------------------------------------------

// 严格解析 mm:ss:ff。mm 允许 1-4 位（个别整轨超过 99 分钟），ss/ff 允许 1-2 位数字，
// 但**范围必须合法**：ss <= 59、ff <= 74（ff=75 已经是下一秒，属于越界脏数据）。
inline bool cue_parse_mmssff(const std::string& in, long long& frames75) {
    const size_t p1 = in.find(':');
    if (p1 == std::string::npos) return false;
    const size_t p2 = in.find(':', p1 + 1);
    if (p2 == std::string::npos) return false;
    const std::string a = in.substr(0, p1);
    const std::string b = in.substr(p1 + 1, p2 - p1 - 1);
    const std::string c = in.substr(p2 + 1);
    auto digits = [](const std::string& s, size_t lo, size_t hi) {
        if (s.size() < lo || s.size() > hi) return false;
        for (char ch : s) if (ch < '0' || ch > '9') return false;
        return true;
    };
    if (!digits(a, 1, 4) || !digits(b, 1, 2) || !digits(c, 1, 2)) return false;
    const int mm = std::atoi(a.c_str());
    const int ss = std::atoi(b.c_str());
    const int ff = std::atoi(c.c_str());
    if (ss > 59 || ff > 74) return false;
    frames75 = ((long long)mm * 60 + ss) * 75 + ff;
    return true;
}

// 1/75 秒帧 → 采样帧（四舍五入到最近采样点；整数运算，无浮点漂移）
//   sampleRate <= 0 或负偏移 → -1（调用方据此判定「偏移未知」）
inline long long cue_frames_to_samples(long long frames75, int sampleRate) {
    if (frames75 < 0 || sampleRate <= 0) return -1;
    return (frames75 * (long long)sampleRate + 37) / 75;      // +37 = 75/2 向下取整，四舍五入
}

inline double cue_frames_to_sec(long long frames75) {
    return frames75 < 0 ? -1.0 : (double)frames75 / 75.0;
}

// ---------------------------------------------------------------------------
// 三、数据结构
// ---------------------------------------------------------------------------

struct CueTrack {
    std::string file;            // 该轨所属的物理音频文件（绝对路径，UTF-8；已按 cue 目录解析）
    std::string fileType;        // FILE 的第二参数（WAVE / MP3 / BINARY …，大写）
    int         number = 0;      // TRACK 号（01..99，原始值）
    std::string type;            // AUDIO / MODE1/2352 / MODE2/2352 …（大写）
    bool        audio = true;    // type == "AUDIO"
    long long   index0 = -1;     // INDEX 00（1/75 秒帧；-1 = 该轨没有 INDEX 00）
    long long   index1 = -1;     // INDEX 01（曲目起点；-1 = 缺失，后处理会丢弃该轨或用 00 降级）
    long long   pregap = -1;     // PREGAP（只记录，不参与偏移）
    long long   postgap = -1;    // POSTGAP（只记录，不参与偏移）
    std::string title, performer, songwriter, isrc;
    std::string albumTitle, albumPerformer, genre, date;   // 全局字段快照（便于单条使用）
    bool index1FromIndex0 = false;  // true = 该轨只有 INDEX 00，index1 是降级来的

    bool hasIndex0() const { return index0 >= 0; }
    double startSec() const { return cue_frames_to_sec(index1); }
};

struct CueSheet {
    std::string cuePath;         // 本份 .cue 的绝对路径（UTF-8）
    std::string albumTitle;      // 全局 TITLE
    std::string performer;       // 全局 PERFORMER
    std::string genre;           // REM GENRE
    std::string date;            // REM DATE / REM YEAR（原样字符串，取年份由调用方决定）
    std::string catalog;         // CATALOG
    std::string discId;          // REM DISCID（EAC 常见，只记录）
    std::vector<CueTrack> tracks;      // 已丢弃「没有 INDEX」「出现在 FILE 之前」的坏轨
    std::vector<std::string> errors;   // 结构性问题（有轨道被丢弃时必有一条）
    std::vector<std::string> warnings; // 无损性问题（未知指令、INDEX 00 降级等）

    bool empty() const { return tracks.empty(); }
    size_t audioCount() const {
        size_t n = 0;
        for (const auto& t : tracks) if (t.audio) ++n;
        return n;
    }
    // 第一处结构错误（便于日志/测试报错）
    std::string firstError() const { return errors.empty() ? std::string() : errors.front(); }
};

// ---------------------------------------------------------------------------
// 四、解析
// ---------------------------------------------------------------------------

inline std::string cue_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (a < b && sp(s[a])) ++a;
    while (b > a && sp(s[b - 1])) --b;
    return s.substr(a, b - a);
}

inline std::string cue_upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

inline std::string cue_pad2(int n) {
    char b[16];
    std::snprintf(b, sizeof(b), "%02d", n);
    return std::string(b);
}

// 取第一个 token：引号内原样（CUE 没有转义），否则到空白为止；restPos 返回 token 之后的位置
inline std::string cue_first_token(const std::string& s, size_t* restPos) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    std::string out;
    if (i < n && (s[i] == '"' || s[i] == '\'')) {
        const char q = s[i++];
        while (i < n && s[i] != q) out += s[i++];
        if (i < n) ++i;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (restPos) *restPos = i;
        return out;
    }
    while (i < n && s[i] != ' ' && s[i] != '\t') out += s[i++];
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (restPos) *restPos = i;
    return out;
}

inline bool cue_all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// FILE 里的相对路径按 **cue 文件所在目录** 解析；分隔符统一成 '\\'（cue 里写 / 的很多）
inline std::string cue_resolve_path(const std::string& cuePath, const std::string& ref) {
    std::string f = ref;
    for (auto& c : f) if (c == '/') c = '\\';
    fsc::path p = cue_u8path(f);
    if (p.is_relative()) {
        const fsc::path dir = cue_u8path(cuePath).parent_path();
        if (!dir.empty()) p = dir / p;
    }
    p = p.lexically_normal();
    std::string out = cue_path_utf8(p);
    while (!out.empty() && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    return out;
}

// 解析已经解码成 UTF-8 的文本。cuePath 只用于解析 FILE 相对路径与日志。
inline CueSheet cue_parse_text(const std::string& decoded, const std::string& cuePath) {
    CueSheet sh;
    sh.cuePath = cuePath;

    std::string text = decoded;
    text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());

    std::string curFile, curFileType;
    CueTrack* cur = nullptr;            // 指向 sh.tracks.back()；只在下一条 TRACK 时重置
    size_t pos = 0;
    int lineNo = 0;

    while (pos <= text.size()) {
        const size_t e = text.find('\n', pos);
        const bool last = (e == std::string::npos);
        std::string line = last ? text.substr(pos) : text.substr(pos, e - pos);
        pos = last ? text.size() + 1 : e + 1;
        ++lineNo;
        line = cue_trim(line);
        if (line.empty()) continue;
        if (line.size() > 4096) {
            sh.warnings.push_back("第 " + std::to_string(lineNo) + " 行超长，已忽略");
            continue;
        }

        const size_t sp = line.find_first_of(" \t");
        const std::string cmd = cue_upper(line.substr(0, sp));
        const std::string arg = (sp == std::string::npos) ? std::string() : cue_trim(line.substr(sp + 1));

        // ---- 元数据（全局 / track 级）----
        if (cmd == "REM") {
            if (cur) continue;                       // 轨内 REM（REPLAYGAIN_* 等）一律忽略
            const size_t p2 = arg.find_first_of(" \t");
            const std::string key = cue_upper(arg.substr(0, p2));
            const std::string val = (p2 == std::string::npos) ? std::string()
                                                             : cue_trim(arg.substr(p2 + 1));
            std::string v = val;
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = cue_trim(v.substr(1, v.size() - 2));
            if      (key == "GENRE")  { if (sh.genre.empty()) sh.genre = v; }
            else if (key == "DATE")   { if (sh.date.empty()) sh.date = v; }
            else if (key == "YEAR")   { if (sh.date.empty()) sh.date = v; }
            else if (key == "ALBUM")  { if (sh.albumTitle.empty()) sh.albumTitle = v; }
            else if (key == "DISCID") { if (sh.discId.empty()) sh.discId = v; }
            continue;
        }
        if (cmd == "CATALOG") { if (!cur) sh.catalog = cue_first_token(arg, nullptr); continue; }
        if (cmd == "CDTEXTFILE" || cmd == "FLAGS") continue;      // 只记录不解释

        if (cmd == "FILE") {
            cur = nullptr;                            // FILE 之后、下个 TRACK 之前的 TITLE/PERFORMER 属专辑
            size_t rp = 0;
            const std::string name = cue_first_token(arg, &rp);
            const std::string typ  = cue_upper(cue_first_token(arg.substr(rp), nullptr));
            if (name.empty()) {
                sh.errors.push_back("第 " + std::to_string(lineNo) + " 行 FILE 没有文件名");
                curFile.clear(); curFileType.clear();
                continue;
            }
            curFileType = typ;
            curFile = cue_resolve_path(cuePath, name);
            if (curFile.empty())
                sh.errors.push_back("第 " + std::to_string(lineNo) + " 行 FILE 路径无法解析：" + name);
            continue;
        }

        if (cmd == "TRACK") {
            size_t rp = 0;
            const std::string numTok = cue_first_token(arg, &rp);
            const std::string typ = cue_upper(cue_trim(arg.substr(rp)));
            if (!cue_all_digits(numTok) || numTok.size() > 3) {
                sh.errors.push_back("第 " + std::to_string(lineNo) + " 行 TRACK 号非法：" + cue_trim(arg));
                cur = nullptr;
                continue;
            }
            CueTrack t;
            t.number   = std::atoi(numTok.c_str());
            t.type     = typ.empty() ? std::string("AUDIO") : typ;   // 省略 TYPE 的方言按 AUDIO 处理
            t.audio    = (t.type == "AUDIO");
            t.file     = curFile;
            t.fileType = curFileType;
            sh.tracks.push_back(std::move(t));
            cur = &sh.tracks.back();
            continue;
        }

        if (cmd == "TITLE") {
            if (cur) cur->title = cue_first_token(arg, nullptr);
            else     sh.albumTitle = cue_first_token(arg, nullptr);
            continue;
        }
        if (cmd == "PERFORMER") {
            if (cur) cur->performer = cue_first_token(arg, nullptr);
            else     sh.performer = cue_first_token(arg, nullptr);
            continue;
        }
        if (cmd == "SONGWRITER") {
            if (cur) cur->songwriter = cue_first_token(arg, nullptr);
            continue;
        }
        if (cmd == "ISRC") {
            if (cur) cur->isrc = cue_first_token(arg, nullptr);
            continue;
        }

        if (cmd == "INDEX") {
            if (!cur) { sh.warnings.push_back("第 " + std::to_string(lineNo) + " 行 INDEX 出现在 TRACK 之前，已忽略"); continue; }
            size_t rp = 0;
            const std::string numTok = cue_first_token(arg, &rp);
            const std::string timeTok = cue_first_token(arg.substr(rp), nullptr);
            if (!cue_all_digits(numTok)) {
                sh.warnings.push_back("第 " + std::to_string(lineNo) + " 行 INDEX 号非法，已忽略：" + cue_trim(arg));
                continue;
            }
            long long f75 = 0;
            if (!cue_parse_mmssff(timeTok, f75)) {
                sh.errors.push_back("第 " + std::to_string(lineNo) + " 行 INDEX 时间非法（要求 mm:ss:ff，ss<=59、ff<=74）：" + timeTok);
                continue;
            }
            const int ix = std::atoi(numTok.c_str());
            if      (ix == 0) cur->index0 = f75;
            else if (ix == 1) cur->index1 = f75;
            else sh.warnings.push_back("第 " + std::to_string(lineNo) + " 行 INDEX " + numTok + " 非 00/01，已忽略");
            continue;
        }
        if (cmd == "PREGAP" || cmd == "POSTGAP") {
            if (!cur) continue;
            long long f75 = 0;
            if (!cue_parse_mmssff(cue_first_token(arg, nullptr), f75)) {
                sh.warnings.push_back("第 " + std::to_string(lineNo) + " 行 " + cmd + " 时间非法，已忽略");
                continue;
            }
            if (cmd == "PREGAP") cur->pregap = f75; else cur->postgap = f75;
            continue;
        }

        sh.warnings.push_back("第 " + std::to_string(lineNo) + " 行未知指令「" + cmd + "」，已忽略");
    }

    // ---- 后处理 ----
    // ① 补齐全局快照（全局 TITLE 写在 TRACK 之后的脏 cue 也能拿到专辑名）
    // ② INDEX 01 缺失、只有 INDEX 00 → 降级用 INDEX 00 当起点（并留警告）
    // ③ 没有任何 INDEX、或出现在 FILE 之前的轨道 → 丢弃（记 error），不让它污染库
    std::vector<CueTrack> keep;
    keep.reserve(sh.tracks.size());
    for (auto& t : sh.tracks) {
        if (t.albumTitle.empty())     t.albumTitle = sh.albumTitle;
        if (t.albumPerformer.empty()) t.albumPerformer = sh.performer;
        if (t.genre.empty())          t.genre = sh.genre;
        if (t.date.empty())           t.date = sh.date;
        if (t.index1 < 0 && t.index0 >= 0) {
            t.index1 = t.index0;
            t.index1FromIndex0 = true;
            sh.warnings.push_back("TRACK " + cue_pad2(t.number) + " 只有 INDEX 00、没有 INDEX 01 → 降级用 INDEX 00 当起点");
        }
        if (t.file.empty()) {
            sh.errors.push_back("TRACK " + cue_pad2(t.number) + " 出现在 FILE 之前，无法确定音频文件 → 丢弃");
            continue;
        }
        if (t.index1 < 0) {
            sh.errors.push_back("TRACK " + cue_pad2(t.number) + " 没有任何 INDEX → 丢弃");
            continue;
        }
        keep.push_back(std::move(t));
    }
    sh.tracks.swap(keep);
    if (sh.tracks.empty() && sh.errors.empty())
        sh.errors.push_back("没有解析出任何 TRACK（文件为空？）");
    return sh;
}

inline bool cue_read_raw(const std::string& path, std::string& out) {
    std::ifstream f(cue_u8path(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

inline CueSheet cue_parse_file(const std::string& cuePath) {
    std::string raw;
    if (!cue_read_raw(cuePath, raw)) {
        CueSheet sh;
        sh.cuePath = cuePath;
        sh.errors.push_back("读不到文件：" + cuePath);
        return sh;
    }
    if (raw.size() > (8u << 20)) {                 // 8MB：正常 cue 只有几 KB
        CueSheet sh;
        sh.cuePath = cuePath;
        sh.errors.push_back("cue 文件过大（" + std::to_string(raw.size()) + " 字节），已拒绝");
        return sh;
    }
    return cue_parse_text(cue_decode_text(raw), cuePath);
}

} // namespace rta::meta
