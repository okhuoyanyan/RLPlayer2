// replaygain.h — ReplayGain / Album Gain（标签读取 + RG2.0 计算 + 三模式应用策略）
// ============================================================================
// ⚠️ 先说结论（给界面和接口用）：**开启后不是 bit-perfect**。
//    ReplayGain 会逐个样本乘以一个非 1.0 的增益，样本值必然改变；用户 261027-01 #2 已
//    明确接受这一点（「虽然会破坏 bit-perfect，但是确实保障统一听感响度的好方法」）。
//    因此本模块**不为了保 bit-perfect 而阉割功能**，而是：
//      * 模式 off（默认）⟹ 增益恒为 0 dB ⟹ apply_gain() 直接短路返回，逐位直通；
//      * 模式 track / album ⟹ 真的乘增益，同时由调用方把 kBitPerfectWarning 显示在界面上。
//    界面文案用 kBitPerfectWarning / kModeLabel[]，不要自己另写一套措辞。
// ============================================================================
//
// ---------------------------------------------------------------------------
// 【接入点：给 tools/rtaweb.cpp 的主人的说明（本模块不碰 rtaweb.cpp）】
//
// 1) 设置项（设置-输出）：新增两项
//      rg.mode      : off | track | album      （默认 off —— 默认必须保持直通）
//      rg.preventClipping : bool               （默认 true）
//    UI 上 mode != off 时把 kBitPerfectWarning 显示出来（红字/警示图标均可）。
//
// 2) 播放路径（每首歌起播、建立 PipelineConfig 的地方，即现在填写 cfg.volumeDb /
//    cfg.loudnessEnabled 的那一段上下文里）：
//      // 起播前：拿到"这首歌"的 ReplayGain 判定（标签优先，缺标签时才用缓存/计算值）
//      rta::dsp::replaygain::Track tr;
//      rta::dsp::replaygain::read_tags(trackPath, &tr.tags, &tr.title, ...);   // 只读标签
//      tr.meas = 缓存里这首歌的 LUFS/峰值（rtahealth 的体检结果；没有就留 valid=false）
//      auto albums = rta::dsp::replaygain::compute_albums(allTracksOfThisAlbum, ref);  // album 模式才要
//      auto dec = rta::dsp::replaygain::decide(tr, cfgMode, preventClipping, albums, ref);
//      cfg.replayGainDb = dec.appliedDb;      // 需要你在 PipelineConfig 里加这一个 double
//      st.rgApplied = dec.active;             // /api/status 里报出来（便于取证）
//
// 3) 真正乘增益的那一段（Pipeline::process 里，**升频之前**）：
//      见下方【谁先谁后】。只有 dec.appliedDb != 0 时才允许乘；
//      appliedDb == 0 时必须完全跳过（不要乘 1.0，"直通"与"乘 1.0"在 -0.0/NaN 上不等价）。
//
// 4) loudness 的联动（一行）：若希望等响度补偿跟着"实际听音电平"走，
//    Loudness::set_volume_db(cfg.volumeDb + dec.appliedDb)，用 loudness_volume_db() 算。
//    ⚠️ 不要反过来把 applied 再喂回 gain（那才会双倍）。
//
// 5) /api/status：建议增 rg="off|track|album"、rgDb=<appliedDb>、rgClip=<clipLimited>。
// ---------------------------------------------------------------------------
//
// ---------------------------------------------------------------------------
// 【支持的容器与实际读到的键名】（2026-10-27 用 ffmpeg 写出的真实夹具逐个实测，
//   见 tests/test_replaygain.cpp [2]；报告里的键名是**原始字符串**，未经改写）
//
//   容器            标签区          track gain 实际键名                 备注
//   FLAC            VORBIS_COMMENT  "REPLAYGAIN_TRACK_GAIN"            大写；ffmpeg/metadatics 都这么写
//   OGG(Vorbis)     stream comment  "REPLAYGAIN_TRACK_GAIN"            在 **stream** 级不在 format 级
//   MP3             ID3v2.3 TXXX    desc="REPLAYGAIN_TRACK_GAIN"       编码字节 0 = latin1；值尾有 NUL
//   MP3/APE/WV      APEv2 (文件尾)  "REPLAYGAIN_TRACK_GAIN"            footer 反着找，值可为 UTF-8
//   M4A/MP4         ilst freeform   "----:com.apple.iTunes:replaygain_track_gain"  小写（iTunes 惯例）
//   WAV             "id3 " 块      ID3v2 TXXX（同上）                   本库的 WAV 就有 id3 块
//   DSF             ID3v2 @ 元数据指针（偏移 20 处 8 字节 LE）
//   DFF             ID3v2 @ 文件尾
//   键名匹配一律先做「去首尾空白 + ASCII 大写 + '-'→'_'」再比较，所以
//   replaygain_track_gain / REPLAYGAIN-TRACK-GAIN / ReplayGain_Track_Gain 都命中；
//   同时接受简写 TRACK_GAIN / ALBUM_GAIN（部分老 tagger 会这么写）。
//   音轨/专辑归属字段也一并读出：TITLE/ARTIST/ALBUM/ALBUMARTIST（MP4: ©nam/©ART/©alb/aART）。
//
// ---------------------------------------------------------------------------
// 【ReplayGain 2.0 的参考电平 —— 出处与口径】
//   ReplayGain 2.0 规范（"ReplayGain 2.0 specification"，Hydrogenaudio wiki；
//   foobar2000 / loudgain / rsgain 的实现都以此为准）把**目标响度定为 −18 LUFS**
//   （对应 RG1.0 的 89 dB SPL 参考），并且响度用 ITU-R BS.1770（K 加权 + 门限）测量。
//   ⟹ 换算公式（本模块唯一口径）：
//         gain_dB = referenceLufs − LUFS_integrated,   referenceLufs 默认 −18.0
//   注意口径差异（别拿错参考去比差）：
//     * EBU R128 广播目标 −23 LUFS；ATSC A/85 −24 LKFS；ffmpeg loudnorm 默认 −24。
//     * RG1.0 的 89 dB 是"声压级"参考，与本公式不是同一条轴，只能通过 −18 LUFS 近似对齐。
//     * 因此"我们的计算值 vs 文件里的标签值"差 5 dB 时，**先怀疑标签是 −23 口径**，
//       不要怀疑 LUFS 分析器（其官方校准点误差 ±0.003 LU）。
//   referenceLufs 做成参数，UI 若想跟随 EBU 就传 −23.0（不推荐，会和绝大多数播放器不一致）。
// ============================================================================
//
// ---------------------------------------------------------------------------
// 【与等响度补偿 loudness.h 的关系：谁先谁后、会不会双倍】
//
//   ReplayGain（本模块） = **曲目/专辑级静态基准**：整曲一个常数增益，补偿"母带做得响不响"。
//   loudness.h           = **音量相关的动态补偿**：低架 + 高架，补偿"人耳在小音量下听不到低频"。
//   两者物理意义正交（一个是宽带常数，一个是按音量变化的频响形状），所以是**串联**，不是叠加同一件事。
//
//   顺序（与 pipeline.h 现有阶段一致，不要改）：
//       解码 → [ReplayGain 增益] → PEQ/EQ → 卷积 → 升频 → [loudness 架式滤波] → [音量] → 量化
//       ^^^^^^^^^^^^^^^^^^^^^ 放在最靠输入的一级，理由见下                      ^^^^ 现状
//     ReplayGain 放在升频之前/PEQ 之前的两条硬理由：
//       a) 防削波判据用的是**源信号峰值**（标签里的 peak 就是源峰值）。若在升频之后才乘，
//          插值过冲已经产生，-peak_dBFS 这条界就不成立了；
//       b) 常数增益与线性时不变处理可交换，但**与量化器不可交换** —— 必须在抖动/整形之前。
//
//   会不会双倍提升？不会，判据有三条（tests/test_replaygain.cpp [6] 实测）：
//     1) loudness 的提升量只由 **cfg.volumeDb（音量旋钮）**决定，不看 ReplayGain 后的电平，
//        两个模块之间**没有反馈回路**，所以不存在"RG 压低 → loudness 再加回来"的迭代放大；
//     2) 串联合成：中频总增益 = rgDb + volumeDb（逐位可验），低频/高频额外那几 dB 来自
//        loudness 的架式滤波，是**频响形状**而不是宽带电平；
//     3) 唯一的量纲耦合是**听感位置**：RG 把电平降了 G dB 之后，用户耳朵听到的是
//        volumeDb + G，而 loudness 仍按 volumeDb 插值 ⟹ 补偿量会偏小（欠补偿，绝不会过量）。
//        要让它跟着实际电平走，用 loudness_volume_db(volumeDb, rgDb) 把 G 加进去 —— 这是
//        **一次性的显式换算**，不是反馈。
//
//   还有一条边界：ReplayGain 的防削波只保"源峰值不越 0 dBFS"。若后级还有 EQ +X dB 提升、
//   或升频带来 inter-sample 过冲，仍需 pipeline 的 preAttenDb / ISP 保护（互不替代）。
// ---------------------------------------------------------------------------
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace rta::dsp::replaygain {

// ---------------------------------------------------------------------------
// 枚举与文案
// ---------------------------------------------------------------------------
enum class Mode { Off = 0, Track = 1, Album = 2 };

inline const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Track: return "track";
        case Mode::Album: return "album";
        default:          return "off";
    }
}
inline const char* mode_label(Mode m) {          // 界面用中文
    switch (m) {
        case Mode::Track: return "ReplayGain（逐曲）";
        case Mode::Album: return "Album Gain（整专辑）";
        default:          return "关闭（bit-perfect 直通）";
    }
}
inline bool parse_mode(const std::string& s, Mode* out) {
    std::string k;
    for (char c : s) if (!std::isspace((unsigned char)c)) k.push_back((char)std::tolower((unsigned char)c));
    if (k == "off" || k == "0" || k == "none" || k == "false") { *out = Mode::Off; return true; }
    if (k == "track" || k == "song" || k == "1")               { *out = Mode::Track; return true; }
    if (k == "album" || k == "2")                              { *out = Mode::Album; return true; }
    return false;
}

// ⚠️ 界面上只要 mode != Off 就必须显示这句话（用户已明确接受破坏 bit-perfect，但不能瞒着）。
inline constexpr const char* kBitPerfectWarning =
    "ReplayGain/Album Gain 会改变样本值：开启后**不是 bit-perfect**（默认关闭时逐位直通）。";

// ---------------------------------------------------------------------------
// 值解析
// ---------------------------------------------------------------------------
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}
inline std::string upper_ascii(std::string s) {
    for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}
// 键名规范化：去空白 + 大写 + '-' → '_'（容器之间拼写差异对消，见文件头表格）
inline std::string norm_key(const std::string& s) {
    std::string k;
    for (char c : s) {
        if ((unsigned char)c <= ' ') continue;
        if (c == '-') c = '_';
        k.push_back(c);
    }
    return upper_ascii(k);
}

// "+3.20 dB" / "-7.50 dB" / "−7,50dB"（含 U+2212 减号与小数逗号）→ -7.5
inline bool parse_gain_db(const std::string& raw, double* out) {
    std::string s = trim(raw);
    if (s.empty()) return false;
    size_t i = 0;
    std::string num;
    bool neg = false;
    // 前导符号：ASCII '-' '+' 或 UTF-8 的 U+2212（E2 88 92）
    if (i + 2 < s.size() && (unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x88 &&
        (unsigned char)s[i + 2] == 0x92) { neg = true; i += 3; }
    else if (s[i] == '-') { neg = true; ++i; }
    else if (s[i] == '+') { ++i; }
    bool digits = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') { num.push_back(c); digits = true; }
        else if ((c == '.' || c == ',') && num.find('.') == std::string::npos) num.push_back('.');
        else break;
    }
    if (!digits) return false;
    double v = std::atof(num.c_str());
    *out = neg ? -v : v;
    return true;
}

// 峰值：ReplayGain 规范的 peak 是**线性**样本峰值（0.988553），不是 dB。
// 兼容两种写坏的变体：① 写成 "-0.10 dB" ⟹ 按 dBFS 解释再转线性；② 写成 "98.8%" ⟹ 百分比。
inline bool parse_peak_linear(const std::string& raw, double* out) {
    const std::string s = trim(raw);
    if (s.empty()) return false;
    std::string low = s; for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.find("db") != std::string::npos) {
        double db = 0.0;
        if (!parse_gain_db(s, &db)) return false;
        *out = std::pow(10.0, db / 20.0);
        return true;
    }
    if (!low.empty() && low.back() == '%') {
        double pct = 0.0;
        if (!parse_gain_db(s.substr(0, s.size() - 1), &pct)) return false;
        *out = pct / 100.0;
        return true;
    }
    double v = 0.0;
    if (!parse_gain_db(s, &v)) return false;   // 纯数字走同一条数字解析
    *out = v;
    return true;
}
inline double linear_to_dbfs(double p) { return (p > 0.0) ? 20.0 * std::log10(p) : -300.0; }

// ---------------------------------------------------------------------------
// 标签读取
// ---------------------------------------------------------------------------
enum class Container { None, Vorbis, ApeV2, Id3v2, Mp4 };

inline const char* container_name(Container c) {
    switch (c) {
        case Container::Vorbis: return "vorbis";
        case Container::ApeV2:  return "apev2";
        case Container::Id3v2:  return "id3v2";
        case Container::Mp4:    return "mp4";
        default:                return "none";
    }
}

// 一次读标签的所有结果。raw* 是**原始字符串**（含 " dB"），key* 是**实际读到的键名**
// （诊断用：报告里要列出"每种容器实际读到的键名"，就是这四个）。
struct TagSet {
    bool ok = false;
    Container container = Container::None;  // **ReplayGain 键所在**的那个标签区（混用时以它为准）
    std::string why;                        // 失败/降级原因（人话）
    std::string containerNote;              // 例如"ID3v2 无 RG 键，RG 来自 APEv2"

    std::string trackGainRaw, trackPeakRaw, albumGainRaw, albumPeakRaw;
    std::string keyTrackGain, keyTrackPeak, keyAlbumGain, keyAlbumPeak;

    // 专辑归组与显示用
    std::string title, artist, album, albumArtist;

    bool has_track_gain() const { return !trackGainRaw.empty(); }
    bool has_track_peak() const { return !trackPeakRaw.empty(); }
    bool has_album_gain() const { return !albumGainRaw.empty(); }
    bool has_album_peak() const { return !albumPeakRaw.empty(); }
    bool any() const { return has_track_gain() || has_track_peak() || has_album_gain() || has_album_peak(); }
};

// 只读随机访问文件（dsp 层不复用 meta/tags_native.h 的 FileReader：那条链会把未知键丢掉，
// 而这里恰恰要拿"未知键"；另外 dsp 不该反向依赖 meta。~30 行，代价可接受。）
class ByteFile {
public:
    bool open(const std::string& utf8) {
        if (utf8.empty()) return false;
        // UTF-8 → 宽字符由 std::filesystem::path 负责（本库文件名人人都含中日文；
        // dsp 层不引 windows.h，走标准库的 path(u8string) 以免和平台头文件打架）
        std::filesystem::path p;
        try {
            p = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
        } catch (...) { return false; }
        f_.open(p, std::ios::binary);
        if (!f_) return false;
        f_.seekg(0, std::ios::end);
        size_ = (long long)f_.tellg();
        return size_ > 0;
    }
    bool ok() const { return (bool)f_ && size_ > 0; }
    long long size() const { return size_; }
    bool read_at(long long off, void* dst, size_t n) const {
        if (off < 0 || n == 0 || off + (long long)n > size_) return false;
        f_.clear(); f_.seekg((std::streamoff)off, std::ios::beg);
        f_.read((char*)dst, (std::streamsize)n);
        return (size_t)f_.gcount() == n;
    }
    size_t read_up_to(long long off, void* dst, size_t n) const {
        if (off < 0 || off >= size_) return 0;
        const size_t want = (size_t)std::min<long long>((long long)n, size_ - off);
        if (!want) return 0;
        f_.clear(); f_.seekg((std::streamoff)off, std::ios::beg);
        f_.read((char*)dst, (std::streamsize)want);
        return (size_t)f_.gcount();
    }
private:
    mutable std::ifstream f_;
    long long size_ = 0;
};

namespace detail {

inline uint32_t le32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
inline uint64_t le64(const unsigned char* p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }
inline uint32_t syncsafe28(const unsigned char* p) { return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14) | ((uint32_t)(p[2] & 0x7f) << 7) | (uint32_t)(p[3] & 0x7f); }

// 把一条 key=value 送进 TagSet（所有容器共用这一个入口 ⟹ 键名口径只有一份）
inline void take_kv(TagSet& t, const std::string& rawKey, const std::string& rawVal,
                    const std::string& displayKey = std::string()) {
    const std::string k = norm_key(rawKey);
    const std::string v = trim(rawVal);
    if (v.empty()) return;
    auto set = [&](std::string& slot, std::string& keySlot) {
        if (slot.empty()) { slot = v; keySlot = displayKey.empty() ? trim(rawKey) : displayKey; }   // 「第一个非空值赢」= ffprobe 口径
    };
    // 标准四键（带 REPLAYGAIN_ 前缀）+ 老式简写（TRACK_GAIN / ALBUM_GAIN）
    if (k == "REPLAYGAIN_TRACK_GAIN" || k == "RG_TRACK_GAIN" || k == "TRACK_GAIN")       set(t.trackGainRaw, t.keyTrackGain);
    else if (k == "REPLAYGAIN_TRACK_PEAK" || k == "RG_TRACK_PEAK" || k == "TRACK_PEAK")  set(t.trackPeakRaw, t.keyTrackPeak);
    else if (k == "REPLAYGAIN_ALBUM_GAIN" || k == "RG_ALBUM_GAIN" || k == "ALBUM_GAIN")  set(t.albumGainRaw, t.keyAlbumGain);
    else if (k == "REPLAYGAIN_ALBUM_PEAK" || k == "RG_ALBUM_PEAK" || k == "ALBUM_PEAK")  set(t.albumPeakRaw, t.keyAlbumPeak);
    // 归属字段（专辑归组用）；同样「第一个非空值赢」
    else if (k == "TITLE")                                          { if (t.title.empty()) t.title = v; }
    else if (k == "ARTIST")                                         { if (t.artist.empty()) t.artist = v; }
    else if (k == "ALBUM")                                          { if (t.album.empty()) t.album = v; }
    else if (k == "ALBUMARTIST" || k == "ALBUM_ARTIST" || k == "ALBUM ARTIST") { if (t.albumArtist.empty()) t.albumArtist = v; }
}

// ---- Vorbis Comment（FLAC 元数据块 / OGG 注释包共用）----
inline bool parse_vorbis_comment(const unsigned char* p, size_t n, TagSet& t, std::string& why) {
    if (n < 8) { why = "vorbis comment 过短"; return false; }
    size_t q = 4;
    const uint32_t vendor = le32(p);
    if (vendor > n - q) { why = "vorbis vendor 越界"; return false; }
    q += vendor;
    if (q + 4 > n) { why = "vorbis 条数缺失"; return false; }
    const uint32_t cnt = le32(p + q);
    q += 4;
    if (cnt > 100000u) { why = "vorbis 条数异常"; return false; }
    for (uint32_t i = 0; i < cnt; ++i) {
        if (q + 4 > n) break;
        const uint32_t len = le32(p + q);
        q += 4;
        if (len > n - q) break;
        const char* s = (const char*)(p + q);
        const void* eq = std::memchr(s, '=', len);
        if (eq) {
            const size_t klen = (size_t)((const char*)eq - s);
            take_kv(t, std::string(s, klen), std::string(s + klen + 1, len - klen - 1));
        }
        q += len;
    }
    t.container = Container::Vorbis;
    return true;
}

// ---- ID3v2：TXXX（desc + value）与 TALB/TPE1/TPE2/TIT2 ----
// enc: 0=latin1 1=utf16(BOM) 2=utf16BE 3=utf8
inline bool id3_decode(const unsigned char* p, size_t n, unsigned char enc, std::string& out, size_t* used) {
    out.clear();
    size_t i = 0;
    auto push_cp = [&](uint32_t cp) {                      // UTF-32 → UTF-8
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    };
    if (enc == 1 || enc == 2) {
        bool big = (enc == 2);
        if (enc == 1 && n >= 2) { if (p[0] == 0xFF && p[1] == 0xFE) { big = false; i = 2; } else if (p[0] == 0xFE && p[1] == 0xFF) { big = true; i = 2; } }
        for (; i + 1 < n; i += 2) {
            const uint32_t w = big ? (((uint32_t)p[i] << 8) | p[i + 1]) : ((uint32_t)p[i] | ((uint32_t)p[i + 1] << 8));
            if (w == 0) { i += 2; break; }
            push_cp(w);
        }
        if (used) *used = i;
        return true;
    }
    // latin1 / utf8：到 NUL 为止
    while (i < n && p[i] != 0) {
        if (enc == 0) push_cp((uint32_t)p[i]);
        else out.push_back((char)p[i]);
        ++i;
    }
    if (i < n) ++i;
    if (used) *used = i;
    return true;
}

inline bool parse_id3v2_at(const ByteFile& f, long long off, long long avail, TagSet& t, std::string& why) {
    unsigned char h[10];
    if (avail < 10 || !f.read_at(off, h, 10)) { why = "id3v2 头读不到"; return false; }
    if (std::memcmp(h, "ID3", 3) != 0) { why = "不是 ID3v2"; return false; }
    const int ver = h[3];
    if (ver < 3) { why = "ID3v2.2 及更早不支持（3 字符帧名）"; return false; }
    const long long tagLen = (long long)syncsafe28(h + 6) + 10;
    if (tagLen <= 0 || tagLen > avail) { why = "id3v2 长度越界"; return false; }
    if (tagLen > (64 << 20)) { why = "id3v2 过大"; return false; }
    std::vector<unsigned char> buf((size_t)tagLen);
    if (!f.read_at(off, buf.data(), (size_t)tagLen)) { why = "id3v2 读失败"; return false; }
    long long p = 10;
    while (p + 10 <= tagLen) {
        const unsigned char* fh = buf.data() + p;
        if (fh[0] == 0 && fh[1] == 0 && fh[2] == 0 && fh[3] == 0) break;   // padding 起点
        char id[5] = {0};
        std::memcpy(id, fh, 4);
        bool idOk = true;
        for (int i = 0; i < 4; ++i) if (!(std::isalnum((unsigned char)id[i]) || id[i] == ' ')) idOk = false;
        if (!idOk) break;
        uint32_t sz = (ver >= 4) ? syncsafe28(fh + 4) : be32(fh + 4);
        if (sz < 0 || p + 10 + (long long)sz > tagLen) break;
        const unsigned char* body = buf.data() + p + 10;
        if (std::memcmp(id, "TXXX", 4) == 0 && sz >= 2) {
            const unsigned char enc = body[0];
            std::string desc, val;
            size_t used = 0;
            id3_decode(body + 1, sz - 1, enc, desc, &used);
            id3_decode(body + 1 + used, sz - 1 - used, enc, val, nullptr);
            take_kv(t, desc, val);           // desc 就是键名（REPLAYGAIN_TRACK_GAIN）
        } else if ((std::memcmp(id, "TALB", 4) == 0 || std::memcmp(id, "TPE1", 4) == 0 ||
                    std::memcmp(id, "TPE2", 4) == 0 || std::memcmp(id, "TIT2", 4) == 0) && sz >= 1) {
            std::string v;
            id3_decode(body + 1, sz - 1, body[0], v, nullptr);
            if (std::memcmp(id, "TALB", 4) == 0)      take_kv(t, "ALBUM", v);
            else if (std::memcmp(id, "TPE1", 4) == 0) take_kv(t, "ARTIST", v);
            else if (std::memcmp(id, "TPE2", 4) == 0) take_kv(t, "ALBUMARTIST", v);
            else                                      take_kv(t, "TITLE", v);
        }
        p += 10 + (long long)sz;
    }
    t.container = Container::Id3v2;
    return true;
}

// ---- APEv2（文件尾；MP3 / APE / WV / MPC 通用）----
// 结构：[items...] [footer 32B] ，footer = "APETAGEX" ver(4) size(4, 含 items 不含 footer)
//        count(4) flags(4) reserved(8)。header 可选（flags bit31 = header present）。
inline bool parse_apev2_at_end(const ByteFile& f, TagSet& t, std::string& why) {
    const long long sz = f.size();
    if (sz < 32) return false;
    auto try_at = [&](long long footerPos, bool* hit) -> bool {
        unsigned char fo[32];
        if (!f.read_at(footerPos, fo, 32)) return false;
        if (std::memcmp(fo, "APETAGEX", 8) != 0) return false;
        *hit = true;
        // ★ APEv2 规范的 size = **含 footer、不含 header** 的字节数（实测 ffmpeg 写的 .wv：
        //   size=291，footer 在 291 里的最后 32 字节，header 另加 32）。第一版按"只含 items"
        //   解析，结果整段错位、一个键都读不出来 —— 这里以实测字节为准。
        const uint32_t tagSize = le32(fo + 12);
        const uint32_t count   = le32(fo + 16);
        const uint32_t flags   = le32(fo + 20);
        if (count > 100000u || tagSize > (64u << 20) || tagSize < 32u) { why = "APEv2 尺寸异常"; return true; }
        // 布局：[header 32B（可选，flags bit31 表示存在）][items ...][footer 32B]
        // size 已包含 footer、不含 header ⟹ items 起点 = footerEnd − size，**不要**再减 header。
        // （第一版多减了 32，正好落在 header 尾部，一个键都读不出来 —— 实测见 .wv 夹具。）
        long long itemsStart = footerPos + 32 - (long long)tagSize;
        const long long itemsLen = (long long)tagSize - 32;
        if (itemsStart < 0 || itemsLen < 0) { why = "APEv2 items 越界"; return true; }
        std::vector<unsigned char> buf((size_t)itemsLen);
        if (itemsLen && !f.read_at(itemsStart, buf.data(), (size_t)itemsLen)) { why = "APEv2 读失败"; return true; }
        size_t q = 0;
        for (uint32_t i = 0; i < count && q + 8 <= buf.size(); ++i) {
            const uint32_t vlen = le32(buf.data() + q);
            const uint32_t iflags = le32(buf.data() + q + 4);
            q += 8;
            const size_t keyStart = q;
            while (q < buf.size() && buf[q] != 0) ++q;
            if (q >= buf.size()) break;
            const std::string key((const char*)buf.data() + keyStart, q - keyStart);
            ++q;                                     // 跳过 NUL
            if (q + vlen > buf.size()) break;
            std::string val((const char*)buf.data() + q, vlen);
            q += vlen;
            if ((iflags & 0x6u) == 0) take_kv(t, key, val);   // 只取文本型（bits1-2 = 类型，0 = UTF-8 文本）
        }
        (void)flags;
        t.container = Container::ApeV2;
        return true;
    };
    // ① 文件尾（可能后面还有 ID3v1 128 字节 / Lyrics3）
    bool hit = false;
    if (try_at(sz - 32, &hit)) return true;
    if (sz >= 32 + 128 && try_at(sz - 32 - 128, &hit)) return true;
    // ② 有的写盘器把 APEv2 放在文件头（罕见）：看开头
    if (sz >= 32) {
        unsigned char hd[32];
        if (f.read_at(0, hd, 32) && std::memcmp(hd, "APETAGEX", 8) == 0) {
            const uint32_t tagSize = le32(hd + 12);
            if (tagSize + 32 <= (uint32_t)sz) { if (try_at((long long)tagSize + 32, &hit)) return true; }
        }
    }
    return false;
}

// ---- MP4 / M4A：moov→udta→meta→ilst→'----'(mean/name/data) ----
inline bool parse_mp4(const ByteFile& f, TagSet& t, std::string& why) {
    const long long sz = f.size();
    // 顶层遍历找 moov
    long long off = 0;
    long long moovOff = -1, moovEnd = -1;
    while (off + 8 <= sz) {
        unsigned char h[16];
        if (!f.read_at(off, h, 8)) break;
        long long asz = (long long)be32(h);
        const uint32_t type = be32(h + 4);
        long long hdr = 8;
        if (asz == 1) { unsigned char e[8]; if (!f.read_at(off + 8, e, 8)) break; asz = (long long)(((uint64_t)be32(e) << 32) | be32(e + 4)); hdr = 16; }
        else if (asz == 0) asz = sz - off;
        if (asz < hdr) break;
        if (type == 0x6D6F6F76u /*moov*/) { moovOff = off + hdr; moovEnd = off + asz; break; }
        off += asz;
    }
    if (moovOff < 0) { why = "mp4: 没有 moov"; return false; }

    // 递归（限定 3 层：udta→meta→ilst 就够）
    struct Found { long long ilstOff = -1, ilstEnd = -1; };
    Found fd;
    auto walk = [&](auto&& self, long long p, long long end, int depth) -> void {
        if (depth > 4 || fd.ilstOff >= 0) return;
        while (p + 8 <= end) {
            unsigned char h[16];
            if (!f.read_at(p, h, 8)) return;
            long long asz = (long long)be32(h);
            const uint32_t type = be32(h + 4);
            long long hdr = 8;
            if (asz == 1) { unsigned char e[8]; if (!f.read_at(p + 8, e, 8)) return; asz = (long long)(((uint64_t)be32(e) << 32) | be32(e + 4)); hdr = 16; }
            else if (asz == 0) asz = end - p;
            if (asz < hdr || p + asz > end) return;
            if (type == 0x696C7374u /*ilst*/) { fd.ilstOff = p + hdr; fd.ilstEnd = p + asz; return; }
            if (type == 0x75647461u /*udta*/) self(self, p + hdr, p + asz, depth + 1);
            else if (type == 0x6D657461u /*meta*/) self(self, p + hdr + 4, p + asz, depth + 1);   // meta 有 4B version/flags
            p += asz;
        }
    };
    walk(walk, moovOff, moovEnd, 0);
    if (fd.ilstOff < 0) { why = "mp4: 没有 ilst"; return false; }

    long long p = fd.ilstOff;
    while (p + 8 <= fd.ilstEnd) {
        unsigned char h[8];
        if (!f.read_at(p, h, 8)) break;
        long long asz = (long long)be32(h);
        const uint32_t type = be32(h + 4);
        if (asz < 8 || p + asz > fd.ilstEnd) break;
        const long long bodyOff = p + 8, bodyEnd = p + asz;
        if (type == 0x2D2D2D2Du /*'----'*/) {
            std::string mean, name, data;
            long long q = bodyOff;
            while (q + 8 <= bodyEnd) {
                unsigned char ch[8];
                if (!f.read_at(q, ch, 8)) break;
                long long csz = (long long)be32(ch);
                const uint32_t ctype = be32(ch + 4);
                if (csz < 8 || q + csz > bodyEnd) break;
                const long long cl = csz - 8;
                if (ctype == 0x6D65616Eu /*mean*/ || ctype == 0x6E616D65u /*name*/) {
                    std::vector<char> b((size_t)std::max<long long>(0, cl - 4));   // 4B version/flags
                    if (!b.empty() && f.read_at(q + 8 + 4, b.data(), b.size())) {
                        std::string v(b.begin(), b.end());
                        while (!v.empty() && (unsigned char)v.back() <= ' ') v.pop_back();
                        if (ctype == 0x6D65616Eu) mean = v; else name = v;
                    }
                } else if (ctype == 0x64617461u /*data*/ && cl > 8) {
                    std::vector<char> b((size_t)(cl - 8));                        // 4B type/flags + 4B locale
                    if (f.read_at(q + 8 + 8, b.data(), b.size())) data.assign(b.begin(), b.end());
                }
                q += csz;
            }
            if (!name.empty()) {
                // 诊断里保留**完整自由格式键名**（"----:com.apple.iTunes:replaygain_track_gain"），
                // 因为报告要按"每种容器实际读到的键名"列出来；匹配仍按 name 部分做。
                take_kv(t, name, data, mean.empty() ? ("----:" + name) : ("----:" + mean + ":" + name));
            }
        } else {
            // 常规 ilst 键：©nam / ©ART / ©alb / aART（© = 0xA9）
            auto read_data = [&](std::string& out) {
                long long q = bodyOff;
                while (q + 8 <= bodyEnd) {
                    unsigned char ch[8];
                    if (!f.read_at(q, ch, 8)) return;
                    long long csz = (long long)be32(ch);
                    if (csz < 8 || q + csz > bodyEnd) return;
                    if (be32(ch + 4) == 0x64617461u && csz > 16) {
                        std::vector<char> b((size_t)(csz - 16));
                        if (f.read_at(q + 8 + 8, b.data(), b.size())) out.assign(b.begin(), b.end());
                        return;
                    }
                    q += csz;
                }
            };
            std::string v;
            if (type == 0xA96E616Du /*©nam*/)      { read_data(v); take_kv(t, "TITLE", v); }
            else if (type == 0xA9415254u /*©ART*/) { read_data(v); take_kv(t, "ARTIST", v); }
            else if (type == 0xA9616C62u /*©alb*/) { read_data(v); take_kv(t, "ALBUM", v); }
            else if (type == 0x61415254u /*aART*/) { read_data(v); take_kv(t, "ALBUMARTIST", v); }
        }
        p += asz;
    }
    t.container = Container::Mp4;
    return true;
}

// ---- FLAC ----
inline bool parse_flac(const ByteFile& f, TagSet& t, std::string& why) {
    long long pos = 0;
    unsigned char magic[4];
    if (!f.read_at(0, magic, 4)) { why = "flac: 读不到头"; return false; }
    if (std::memcmp(magic, "ID3", 3) == 0) {                         // 少见但要能跳过
        unsigned char h[10];
        if (!f.read_at(0, h, 10)) return false;
        pos = 10 + (long long)syncsafe28(h + 6);
        if (!f.read_at(pos, magic, 4)) return false;
    }
    if (std::memcmp(magic, "fLaC", 4) != 0) { why = "flac: 不是 fLaC"; return false; }
    long long p = pos + 4;
    bool gotVC = false;
    for (;;) {
        unsigned char b[4];
        if (!f.read_at(p, b, 4)) { why = "flac: 元数据块头越界"; break; }
        const bool last = (b[0] & 0x80) != 0;
        const int type = b[0] & 0x7f;
        const long long len = ((long long)b[1] << 16) | ((long long)b[2] << 8) | (long long)b[3];
        p += 4;
        if (len < 0 || p + len > f.size()) { why = "flac: 块长度越界"; break; }
        if (type == 4 && len >= 8 && len <= (16 << 20)) {            // VORBIS_COMMENT
            std::vector<unsigned char> buf((size_t)len);
            if (f.read_at(p, buf.data(), (size_t)len)) {
                std::string w;
                if (parse_vorbis_comment(buf.data(), buf.size(), t, w)) gotVC = true;
                else why = "flac: " + w;
            }
        }
        p += len;
        if (last) break;
        if (p + 4 > f.size()) { why = "flac: 块链越界"; break; }
    }
    if (!gotVC) { if (why.empty()) why = "flac: 没有 VORBIS_COMMENT"; return false; }
    return true;
}

// ---- OGG（Vorbis/Opus：注释包是第 2 个包，可能跨页；按 lacing 值拼包）----
struct OggPage {
    int headerType = 0;
    long long granule = 0;
    int segments = 0;
    std::vector<unsigned char> lace;   // segments 个 lacing 值
    std::vector<unsigned char> body;
    long long next = 0;
};

inline bool ogg_next_page(const ByteFile& f, long long off, OggPage& pg) {
    unsigned char h[27];
    if (!f.read_at(off, h, 27)) return false;
    if (std::memcmp(h, "OggS", 4) != 0) return false;
    pg.headerType = h[5];
    pg.granule = (long long)le64(h + 6);
    pg.segments = h[26];
    pg.lace.assign((size_t)pg.segments, 0);
    if (pg.segments && !f.read_at(off + 27, pg.lace.data(), (size_t)pg.segments)) return false;
    size_t total = 0;
    for (int i = 0; i < pg.segments; ++i) total += pg.lace[(size_t)i];
    pg.body.assign(total, 0);
    if (total && !f.read_at(off + 27 + pg.segments, pg.body.data(), total)) return false;
    pg.next = off + 27 + pg.segments + (long long)total;
    return true;
}

inline bool parse_ogg(const ByteFile& f, TagSet& t, std::string& why) {
    long long off = 0;
    std::vector<unsigned char> cur;    // 正在拼的包
    int pktIndex = 0;
    for (int guard = 0; guard < 128; ++guard) {      // 注释包总在前几页里
        OggPage pg;
        if (!ogg_next_page(f, off, pg)) { why = "ogg: 页读失败"; break; }
        size_t pos = 0;
        for (int i = 0; i < pg.segments; ++i) {
            const size_t len = pg.lace[(size_t)i];
            if (pos + len > pg.body.size()) { why = "ogg: lacing 越界"; break; }
            cur.insert(cur.end(), pg.body.begin() + (long long)pos, pg.body.begin() + (long long)(pos + len));
            pos += len;
            if (len < 255) {                          // 一个完整包结束
                if (pktIndex == 1) {
                    size_t start = 0;
                    if (cur.size() >= 7 && cur[0] == 0x03 && std::memcmp(cur.data() + 1, "vorbis", 6) == 0) start = 7;
                    else if (cur.size() >= 8 && std::memcmp(cur.data(), "OpusTags", 8) == 0) start = 8;
                    else if (cur.size() >= 8 && std::memcmp(cur.data(), "Speex   ", 8) == 0) start = 8;
                    else { why = "ogg: 第 2 个包不是注释包"; return false; }
                    std::string w;
                    if (!parse_vorbis_comment(cur.data() + start, cur.size() - start, t, w)) {
                        why = "ogg: " + w;
                        return false;
                    }
                    return true;
                }
                cur.clear();
                ++pktIndex;
                if (pktIndex > 1) { why = "ogg: 没有注释包"; return false; }
            }
        }
        off = pg.next;
    }
    if (why.empty()) why = "ogg: 没有注释包";
    return false;
}

// ---- DSF（ID3v2 在文件尾；头 20 处有 8 字节 LE 指针）----
inline bool parse_dsf(const ByteFile& f, TagSet& t, std::string& why) {
    unsigned char h[28];
    if (!f.read_at(0, h, 28) || std::memcmp(h, "DSD ", 4) != 0) { why = "dsf: 头不对"; return false; }
    const uint64_t metaPos = le64(h + 20);
    if (metaPos == 0 || (long long)metaPos >= f.size()) { why = "dsf: 没有元数据指针"; return false; }
    return parse_id3v2_at(f, (long long)metaPos, f.size() - (long long)metaPos, t, why);
}
// ---- DFF（ID3v2 追加在文件尾）----
inline bool parse_dff(const ByteFile& f, TagSet& t, std::string& why) {
    const long long sz = f.size();
    if (sz < 12) return false;
    unsigned char h[12];
    if (!f.read_at(0, h, 12) || std::memcmp(h, "FRM8", 4) != 0) { why = "dff: 头不对"; return false; }
    long long p = 12;
    while (p + 12 <= sz) {
        unsigned char ch[12];
        if (!f.read_at(p, ch, 12)) break;
        uint64_t csz = (uint64_t)le32(ch + 8) << 32 | (uint64_t)le32(ch + 4);
        if (std::memcmp(ch, "ID3 ", 4) == 0 || std::memcmp(ch, "id3 ", 4) == 0)
            return parse_id3v2_at(f, p + 12, sz - (p + 12), t, why);
        p += 12 + (long long)csz + ((csz & 1) ? 1 : 0);   // DFF 块按偶数字节对齐
    }
    why = "dff: 没有 ID3 块";
    return false;
}

inline std::string lower_ext(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t sep = path.find_last_of("/\\");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return "";
    std::string e = path.substr(dot);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

} // namespace detail

// ---------------------------------------------------------------------------
// 对外入口：读一个文件的 ReplayGain 标签（**只读**，绝不写盘、绝不改用户文件）
//   返回 true ⟹ tags.ok = true；四个 raw* / key* 字段按容器填好（可能为空 = 该键不存在）。
//   ⚠️ APEv2 是"文件尾标签"，与容器无关，所以哪怕扩展名不认识也会试一次。
// ---------------------------------------------------------------------------
inline bool read_tags(const std::string& utf8path, TagSet* out, std::string* why = nullptr) {
    if (!out) return false;
    *out = TagSet();
    if (why) why->clear();
    ByteFile f;                             // ByteFile 在 replaygain 命名空间里（不在 detail）
    if (!f.open(utf8path)) { out->why = "文件打不开/为空"; if (why) *why = out->why; return false; }

    const std::string ext = detail::lower_ext(utf8path);
    std::string my;
    bool ok = false;
    if      (ext == ".flac")                     ok = detail::parse_flac(f, *out, my);
    else if (ext == ".ogg" || ext == ".oga" || ext == ".opus") ok = detail::parse_ogg(f, *out, my);
    else if (ext == ".mp3" || ext == ".wav" || ext == ".wave") {
        // 先看 ID3v2（MP3 在头，WAV 在 "id3 " 块里）
        ok = detail::parse_id3v2_at(f, 0, f.size(), *out, my);
        if (!ok && (ext == ".wav" || ext == ".wave")) {
            // WAV：遍历 RIFF 块找 "id3 "
            unsigned char h[12];
            if (f.read_at(0, h, 12) && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVE", 4) == 0) {
                long long p = 12;
                while (p + 8 <= f.size()) {
                    unsigned char ch[8];
                    if (!f.read_at(p, ch, 8)) break;
                    const uint32_t csz = detail::le32(ch + 4);
                    if (std::memcmp(ch, "id3 ", 4) == 0 || std::memcmp(ch, "ID3 ", 4) == 0) {
                        ok = detail::parse_id3v2_at(f, p + 8, (long long)csz, *out, my);
                        break;
                    }
                    p += 8 + (long long)csz + (csz & 1);
                }
                if (!ok) my = "wav: 没有 id3 块";
            }
        }
    }
    else if (ext == ".m4a" || ext == ".mp4" || ext == ".m4b" || ext == ".aac") ok = detail::parse_mp4(f, *out, my);
    else if (ext == ".dsf")                      ok = detail::parse_dsf(f, *out, my);
    else if (ext == ".dff")                      ok = detail::parse_dff(f, *out, my);

    // APEv2：不依赖容器，任何文件都试（MP3 常与 ID3v2 并存，两边的 RG 键都要看）
    if (!out->any()) {
        TagSet ape;
        std::string w2;
        if (detail::parse_apev2_at_end(f, ape, w2)) {
            const bool apeRg = ape.has_track_gain() || ape.has_track_peak() ||
                               ape.has_album_gain() || ape.has_album_peak();
            if (ok) {
                // 合并：ID3v2 里没有的键用 APEv2 补（"第一个非空值赢"仍然成立）
                if (out->trackGainRaw.empty()) { out->trackGainRaw = ape.trackGainRaw; out->keyTrackGain = ape.keyTrackGain; }
                if (out->trackPeakRaw.empty()) { out->trackPeakRaw = ape.trackPeakRaw; out->keyTrackPeak = ape.keyTrackPeak; }
                if (out->albumGainRaw.empty()) { out->albumGainRaw = ape.albumGainRaw; out->keyAlbumGain = ape.keyAlbumGain; }
                if (out->albumPeakRaw.empty()) { out->albumPeakRaw = ape.albumPeakRaw; out->keyAlbumPeak = ape.keyAlbumPeak; }
                if (out->album.empty())     out->album = ape.album;
                if (out->albumArtist.empty()) out->albumArtist = ape.albumArtist;
                if (apeRg) {   // RG 键来自 APE ⟹ container 报 APEv2（报告里要能看出"键从哪来"）
                    out->container = ape.container;
                    out->containerNote = "ID3v2 里没有 RG 键，RG 键来自 APEv2";
                }
            } else {
                *out = ape;
                ok = true;
            }
        } else if (!ok) {
            if (!w2.empty()) my = w2;
        }
    }
    out->ok = ok;
    if (ok) out->why.clear(); else out->why = my.empty() ? std::string("无法解析标签") : my;
    if (why) *why = out->why;
    return ok;
}

// ---------------------------------------------------------------------------
// 缺标签时的计算口径（ReplayGain 2.0：−18 LUFS 参考）
//   gain_dB = referenceLufs − LUFS_integrated
// 调用方（probe / 体检缓存）负责用 analysis::DynamicsAccumulator 量出 LUFS 与峰值，
// 本模块只做换算，保持 dsp 不反向依赖 analysis。
// ---------------------------------------------------------------------------
inline constexpr double kReferenceLufs = -18.0;   // ReplayGain 2.0 目标响度（见文件头出处）
inline double gain_from_lufs(double lufsIntegrated, double referenceLufs = kReferenceLufs) {
    return referenceLufs - lufsIntegrated;
}

// ---- 单文件测量结果（缺标签时由调用方填）----
struct Measurement {
    bool   valid = false;
    double lufs = 0.0;
    double peak = 0.0;         // 线性峰值
    double durationSec = 0.0;  // 专辑加权用；未知填 0（则权重按 1）
};

// ---- 一首歌的全部输入 ----
struct Track {
    std::string path;
    TagSet      tags;
    Measurement meas;
    std::string album, albumArtist;   // 优先取 tags 里的，空则用外部填的

    std::string title_or_file() const {
        if (!tags.title.empty()) return tags.title;
        const size_t sep = path.find_last_of("/\\");
        return sep == std::string::npos ? path : path.substr(sep + 1);
    }
    std::string alb() const { return !tags.album.empty() ? tags.album : album; }
    std::string aart() const { return !tags.albumArtist.empty() ? tags.albumArtist : albumArtist; }
    // 专辑归组键：同 album + 同 albumartist。albumartist 缺失时退回 artist（多歌手合辑的
    // 常见写法是 albumartist=Various Artists，所以有就用它；两边都空 ⟹ 归到"未知专辑"，不猜）。
    std::string album_key() const {
        const std::string a = norm_key(alb());
        std::string ar = aart();
        if (ar.empty()) ar = tags.artist;
        const std::string b = norm_key(ar);
        if (a.empty() && b.empty()) return std::string();
        return a + "\x1f" + b;
    }
    double peak_linear() const {
        double p = 0.0;
        if (tags.has_track_peak() && parse_peak_linear(tags.trackPeakRaw, &p) && p > 0.0) return p;
        if (meas.valid && meas.peak > 0.0) return meas.peak;
        return 0.0;
    }
};

// ---- 专辑归组结果 ----
struct AlbumInfo {
    std::string key, album, albumArtist;
    std::vector<size_t> members;
    double gainDb = 0.0;          // 专辑增益（标签优先，否则由专辑响度换算）
    bool   fromTag = false;
    bool   inconsistentTag = false;   // 组内各曲 album gain 标签不一致（取中位数）
    double lufs = 0.0;            // 专辑整体响度（计算值；标签路径下也尽量给出）
    double peak = 0.0;            // 组内最大峰值（线性）
    bool   havePeak = false;
    bool   exactLufs = false;     // lufs 是否来自"拼接后统一门限"（而非时长加权近似）
    std::string note;
};

// 专辑整体响度：按**时长加权的能量平均**（不是 dB 平均！dB 平均会低估响的曲子）。
//   L_album = −0.691 + 10log10( Σ_i 10^((L_i+0.691)/10)·T_i / Σ_i T_i )
// 与"把整张专辑当一条连续信号跑 BS.1770 门限"的差别 = 门限跨曲目重算（实测见
// tests/test_replaygain.cpp [7] 与报告：真实专辑上差 < 0.2 LU）。没有时长时 T_i = 1。
inline double album_lufs_from_tracks(const std::vector<Track>& tracks, const std::vector<size_t>& idx) {
    double num = 0.0, den = 0.0;
    bool any = false;
    for (size_t i : idx) {
        const Track& t = tracks[i];
        if (!t.meas.valid) continue;
        any = true;
        const double w = (t.meas.durationSec > 0.0) ? t.meas.durationSec : 1.0;
        const double p = std::pow(10.0, (t.meas.lufs + 0.691) / 10.0);   // 线性功率
        num += p * w;
        den += w;
    }
    // ★ 一首都没测过 ⟹ 必须返回 −300（"测不出"），不能返回 0.0 ——
    //   0.0 会被上层当成一个有效的响度（−18 − 0 = −18 dB 的"增益"），那是凭空造数。
    if (!any || den <= 0.0) return -300.0;
    const double p = num / den;
    if (p <= 0.0) return -300.0;
    return -0.691 + 10.0 * std::log10(p);
}

// 建组 + 算专辑增益。ref = 参考响度（默认 −18 LUFS）。
//   albumLufsOverride（可选）：key = album_key()，value = **拼接整张专辑后统一门限**测得的 LUFS。
//   什么时候该用：能离线把整张专辑当一条信号跑一遍时（rg_probe --album-exact、体检缓存）。
//   不传就退回上面的"时长加权能量平均"——它便宜（只用逐曲 LUFS），但与真实专辑口径有偏差
//   （实测极端素材最大 −1.22 LU，见 tests/test_replaygain.cpp [7]；真实专辑见报告）。
inline std::vector<AlbumInfo> compute_albums(const std::vector<Track>& tracks, double ref = kReferenceLufs,
                                             const std::map<std::string, double>* albumLufsOverride = nullptr) {
    std::vector<AlbumInfo> out;
    std::map<std::string, size_t> pos;
    for (size_t i = 0; i < tracks.size(); ++i) {
        std::string k = tracks[i].album_key();
        if (k.empty()) k = std::string("\x1f") + "unknown#" + std::to_string(i);   // 无法归组 ⟹ 自成一组
        auto it = pos.find(k);
        if (it == pos.end()) {
            AlbumInfo a;
            a.key = k;
            a.album = tracks[i].alb();
            a.albumArtist = tracks[i].aart();
            a.members.push_back(i);
            pos[k] = out.size();
            out.push_back(a);
        } else {
            out[it->second].members.push_back(i);
        }
    }
    for (auto& a : out) {
        std::vector<double> tagGains;
        for (size_t i : a.members) {
            const Track& t = tracks[i];
            double g = 0.0;
            if (t.tags.has_album_gain() && parse_gain_db(t.tags.albumGainRaw, &g)) tagGains.push_back(g);
            double pk = 0.0;
            if (t.tags.has_album_peak() && parse_peak_linear(t.tags.albumPeakRaw, &pk) && pk > a.peak) { a.peak = pk; a.havePeak = true; }
            else if (!a.havePeak) {
                const double p = t.peak_linear();
                if (p > a.peak) { a.peak = p; a.havePeak = p > 0.0; }
            }
        }
        double ovLufs = 0.0;
        bool overridden = false;
        if (albumLufsOverride) {
            const auto it = albumLufsOverride->find(a.key);
            if (it != albumLufsOverride->end() && it->second > -299.0) { ovLufs = it->second; overridden = true; }
        }
        a.lufs = overridden ? ovLufs : album_lufs_from_tracks(tracks, a.members);
        if (overridden) a.exactLufs = true;
        if (!tagGains.empty()) {
            std::sort(tagGains.begin(), tagGains.end());
            a.gainDb = tagGains[tagGains.size() / 2];        // 中位数（组内一致时就是那个值）
            a.fromTag = true;
            if (tagGains.back() - tagGains.front() > 0.05) {
                a.inconsistentTag = true;
                a.note = "组内 ALBUM_GAIN 标签不一致（跨度 " +
                         std::to_string(tagGains.back() - tagGains.front()) + " dB），取中位数";
            }
        } else if (a.lufs > -299.0) {
            a.gainDb = gain_from_lufs(a.lufs, ref);
            a.fromTag = false;
        } else {
            a.gainDb = 0.0;
            a.note = "组内没有可用测量值，专辑增益按 0 dB 处理";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 应用判定
// ---------------------------------------------------------------------------
struct Params {
    Mode   mode = Mode::Off;              // 默认 off ⟹ 不动
    double referenceLufs = kReferenceLufs;
    bool   preventClipping = true;        // 默认开
    double minGainDb = -60.0;             // 合理区间钳制（防标签写坏成 "-999 dB"）
    double maxGainDb = 30.0;
};

struct Decision {
    bool   active = false;        // false ⟹ 完全不动（apply_gain 不碰样本）
    double requestedDb = 0.0;     // 标签给的值 或 计算值
    double appliedDb = 0.0;       // 最终增益（已过削波保护与区间钳制）
    bool   clipLimited = false;   // 是否被防削波改过
    bool   fromTag = false;       // requestedDb 的来源：true=标签，false=计算
    bool   fromAlbum = false;     // album 模式
    bool   clamped = false;       // 是否被 min/max 钳过
    bool   havePeak = false;
    double peakLinear = 0.0;
    double peakDbfs = -300.0;
    double maxSafeGainDb = 0.0;   // -peak_dBFS（peak>1 时是负数）
    std::string source;           // "tag:track" / "compute:album" …（日志/JSON 用）
    std::string note;
};

// 单曲判定。albums 只在 mode=Album 时用（可为空 ⟹ 退回逐曲标签/计算值）。
inline Decision decide(const Track& t, const Params& p, const std::vector<AlbumInfo>* albums = nullptr) {
    Decision d;
    if (p.mode == Mode::Off) { d.source = "off"; return d; }        // 直接不动：bit-perfect

    const AlbumInfo* al = nullptr;
    if (p.mode == Mode::Album && albums) {
        const std::string k = t.album_key();
        for (const auto& a : *albums) if (a.key == k) { al = &a; break; }
    }
    d.fromAlbum = (p.mode == Mode::Album);

    // ---- 1) 请求增益：优先级（每一级的"为什么"都写在旁边）----
    //   album 模式：专辑标签 > 专辑计算值 > 逐曲标签（兜底）> 逐曲计算值（兜底）
    //   track 模式：逐曲标签 > 逐曲计算值
    //   兜底顺序的意义：album 模式绝不能因为"这张专辑缺数据"就变成 0 dB 或乱用逐曲值而不作声，
    //   所以每一次降级都写进 d.source / d.note，让界面和日志能看出"这次其实用的是逐曲"。
    double g = 0.0;
    bool haveG = false;
    if (p.mode == Mode::Album && al && al->fromTag) {
        g = al->gainDb; haveG = true; d.fromTag = true; d.source = "tag:album";
    }
    else if (p.mode == Mode::Album && al && al->members.size() && al->lufs > -299.0) {
        g = al->gainDb; haveG = true; d.fromTag = false; d.source = "compute:album";
        if (al->inconsistentTag) d.note = al->note;
    }
    else if (t.tags.has_track_gain() && parse_gain_db(t.tags.trackGainRaw, &g)) {
        haveG = true; d.fromTag = true;
        d.source = (p.mode == Mode::Album) ? "tag:track(fallback)" : "tag:track";
        if (p.mode == Mode::Album) d.note = "拿不到专辑数据（既无 album 标签也无专辑响度），退回逐曲标签";
    }
    else if (t.meas.valid) {
        g = gain_from_lufs(t.meas.lufs, p.referenceLufs); haveG = true; d.fromTag = false;
        d.source = (p.mode == Mode::Album) ? "compute:track(fallback)" : "compute:track";
        if (p.mode == Mode::Album) d.note = "拿不到专辑数据，退回逐曲计算值";
    }
    if (!haveG) {
        d.active = false;
        d.note = "既没有 ReplayGain 标签，也没有可用的 LUFS 测量值 ⟹ 不动（0 dB）";
        d.source = "none";
        return d;
    }
    d.requestedDb = g;

    // ---- 2) 峰值（防削波依据）：标签峰值优先，缺了才用测量峰值 ----
    double pk = 0.0;
    bool hp = false;
    if (p.mode == Mode::Album && al && al->havePeak) { pk = al->peak; hp = true; }
    else if (t.tags.has_track_peak() && parse_peak_linear(t.tags.trackPeakRaw, &pk) && pk > 0.0) hp = true;
    else if (t.meas.valid && t.meas.peak > 0.0) { pk = t.meas.peak; hp = true; }
    d.havePeak = hp;
    d.peakLinear = hp ? pk : 0.0;
    d.peakDbfs = hp ? linear_to_dbfs(pk) : -300.0;

    // ---- 3) 防削波：applied = min(gain, -peak_dBFS) ----
    //   peak_dBFS > 0（源就过载，例如 +0.5 dBFS）时上界是负的 ⟹ 反而要**压低**，这正是我们要的：
    //   ReplayGain 在这类素材上会同时承担"别把过载再放大"的责任。
    double g2 = g;
    d.maxSafeGainDb = hp ? -d.peakDbfs : 1e9;
    if (p.preventClipping && hp) {
        if (g2 > d.maxSafeGainDb + 1e-9) {
            g2 = d.maxSafeGainDb;
            d.clipLimited = true;
            d.note = "防削波：请求 " + std::to_string(g) + " dB，峰值 " + std::to_string(d.peakDbfs) +
                     " dBFS ⟹ 限到 " + std::to_string(g2) + " dB";
        }
    }
    // ---- 4) 区间钳制（标签写坏时的护栏）----
    if (g2 < p.minGainDb) { g2 = p.minGainDb; d.clamped = true; }
    if (g2 > p.maxGainDb) { g2 = p.maxGainDb; d.clamped = true; }

    d.appliedDb = g2;
    d.active = true;
    return d;
}

// ---------------------------------------------------------------------------
// 应用：0 dB 必须逐位直通（不乘 1.0）
// ---------------------------------------------------------------------------
inline void apply_gain(double gainDb, double* buf, size_t n) {
    if (!buf || n == 0) return;
    if (gainDb == 0.0) return;                     // ★ 直通：不碰样本
    const double g = std::pow(10.0, gainDb / 20.0);
    for (size_t i = 0; i < n; ++i) buf[i] *= g;
}
inline void apply_gain(double gainDb, double* const* ch, int channels, size_t n) {
    if (!ch) return;
    for (int c = 0; c < channels; ++c) apply_gain(gainDb, ch[c], n);
}

// ---------------------------------------------------------------------------
// 与 loudness 的联动（见文件头【谁先谁后】第 3 条）
//   把 ReplayGain 造成的电平变化折进音量，好让等响度补偿跟着"实际听音电平"走。
//   ⚠️ 一次性换算，不是反馈：绝不要把 appliedDb 再喂回 ReplayGain。
// ---------------------------------------------------------------------------
inline double loudness_volume_db(double volumeDb, double replayGainDb) { return volumeDb + replayGainDb; }

} // namespace rta::dsp::replaygain
