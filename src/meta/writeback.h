// writeback.h — P2 标签写回(docs/元数据补全调研与规划.md §3/§4 P2:ffmpeg remux + PCM 逐位门禁)
//
// 流程(writeback_file):
//   扩展名门(见下) → 收集非空字段 → ffmpeg remux(-c copy)到同目录 .rtatmp 临时文件 →
//   PCM 门禁:新旧两份解码成同参数 PCM WAV(pcm_s32le)后逐字节比较(含长度) →
//   不一致:删临时、报错、绝不动原文件;一致:dry-run 则删临时返回清单,否则
//   备份(原文件移入 <store>/backups/<hash>.<ext>) + 原子替换(rename 临时→原路径)。
//
// 安全红线(本文件实现):
//   ① 写回一律先写临时文件,校验通过才动原文件;② PCM 校验不一致绝不替换;
//   ③ 绝不写 DSD(.dsf/.dff)与不可识别扩展名;④ 替换任一步失败都把原文件从备份搬回。
//
// 键名映射 —— 2026-10-22 实测(ffmpeg 9.0 full gyan build,样例 remux → ffprobe 读回):
//   * Vorbis 注释家族(flac/ogg/opus/wv)写大写键,读回时 flac/ogg/opus 的
//     ALBUMARTIST/TRACKNUMBER/DISCNUMBER 被 ffprobe 归一为 album_artist/track/disc
//     (与规划文档的括号名一致);wv(APEv2)读回原样大写、不归一。
//   * mp3(ID3v2.4,-write_id3v2 1 -id3v2_version 4)只认小写规范键:大写 Vorbis 名
//     (ALBUMARTIST/TRACKNUMBER/DISCNUMBER/LABEL 等)会变成 TXXX 私有帧而不是
//     TPE2/TRCK/TPOS —— 实测 b.mp3 两键并存。规范键 = title/artist/album/album_artist/
//     date/genre/track/disc/composer/conductor/publisher/lyrics;label→publisher。
//   * m4a(ipod muxer):conductor/label 被静默丢弃(无对应 iTunes atom 映射,实测读回缺);
//     track/disc 被规整为整数(03→3)。
//   * wav(RIFF INFO):仅 title/artist/album/date/genre/track 存活,其余键被 muxer 丢弃。
//   * ape:ffmpeg 9.0 无 APE muxer(实测 `ffmpeg -muxers` 只有解码器)→ 无法无损 remux,跳过。
//   * PCM 门禁实测:上述全部格式 remux 前后解码逐位一致(含 mp3 的 Xing/LAME gapless
//     元数据,ffmpeg 9.0 -c copy 会保留 skip_samples 边界)。
//
// 无异常纪律(与 meta/ 其它头一致):全 error_code / 返回值路径,不抛。
#pragma once
#include "meta/store.h"
#include "platform/fs.h"        // 删除 / 搬移 / 复制 + 系统错误码（写回安全红线全靠这几个操作）
#include "platform/path.h"      // UTF-8 <-> 原生路径、临时目录、小写扩展名
#include "platform/process.h"   // 调 ffmpeg / ffprobe（隐藏窗口、收日志）
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rta::meta {

// ---------------------------------------------------------------------------
// 字段 → metadata 键映射(实测结论见文件头注释)
// ---------------------------------------------------------------------------

// 12 个常规字段的固定顺序(与 store.h TrackMeta 注释、rtatag store set 的约定一致)
inline const char* const* writeback_field_order() {
    static const char* k[] = { "title", "artist", "album", "albumArtist", "year", "genre",
                               "trackNo", "discNo", "composer", "conductor", "label", "lyrics" };
    return k;
}

// Vorbis 注释家族(flac/ogg/opus/wv):大写键;ID3/ASF/MP4 家族(mp3/m4a/wma/wav):小写规范键
inline const char* const* writeback_keys(bool vorbisFamily) {
    static const char* kV[] = { "TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "DATE", "GENRE",
                                "TRACKNUMBER", "DISCNUMBER", "COMPOSER", "CONDUCTOR", "LABEL", "LYRICS" };
    static const char* kG[] = { "title", "artist", "album", "album_artist", "date", "genre",
                                "track", "disc", "composer", "conductor", "publisher", "lyrics" };
    return vorbisFamily ? kV : kG;
}

// 扩展名(小写含点) → ffmpeg muxer 名;不支持返回 nullptr。
// m4a 走 ipod muxer、wma 走 asf muxer(实测 2026-10-22,ffmpeg 9.0)。
inline const char* writeback_muxer(const std::string& ext) {
    if (ext == ".flac") return "flac";
    if (ext == ".mp3")  return "mp3";
    if (ext == ".ogg")  return "ogg";
    if (ext == ".opus") return "opus";
    if (ext == ".m4a")  return "ipod";
    if (ext == ".wma")  return "asf";
    if (ext == ".wav")  return "wav";
    if (ext == ".wv")   return "wv";
    return nullptr;
}

inline bool writeback_vorbis_family(const std::string& ext) {
    return ext == ".flac" || ext == ".ogg" || ext == ".opus" || ext == ".wv";
}

// year 写回守卫(防御纵深,2026-10-22):值必须恰为 4 位数字且落在 [1000,2100]。
// 背景:QQ 源曾对该曲返回 year="0000" 入库,写回侧原样采纳,文件 DATE 从
// 2022-12-24 变 "0000"。provider.h 的 pv_year4 已在源头丢弃窗口外的年份;
// 这里挡 store 里【已存在】的脏值,不让它再外溢进文件 DATE。
inline bool writeback_year_ok(const std::string& y) {
    if (y.size() != 4) return false;
    for (const char c : y) if (c < '0' || c > '9') return false;
    const int v = (y[0] - '0') * 1000 + (y[1] - '0') * 100 +
                  (y[2] - '0') * 10 + (y[3] - '0');
    return v >= 1000 && v <= 2100;
}

// 收集非空字段(按固定字段顺序;空值不写,避免用空串覆盖既有标签)。
// year 守卫:值非合理年份(如 store 里的脏值 "0000")→ 跳过该字段不写,
// 并把说明写入 *yearSkipNote(调用方并入 note;传 nullptr 则只过滤不说明)。
struct WbField {
    const char* field;      // TrackMeta 字段名(title/artist/...)
    const char* key;        // ffmpeg metadata 键(TITLE/album_artist/...)
    std::string value;
};

inline std::vector<WbField> writeback_collect(const TrackMeta& tm, bool vorbisFamily,
                                              std::string* yearSkipNote = nullptr) {
    if (yearSkipNote) yearSkipNote->clear();
    std::vector<WbField> o;
    const char* const* order = writeback_field_order();
    const char* const* keys  = writeback_keys(vorbisFamily);
    for (int i = 0; i < 12; ++i) {
        const auto it = tm.fields.find(order[i]);
        if (it == tm.fields.end() || it->second.value.empty()) continue;
        if (std::string(order[i]) == "year" && !writeback_year_ok(it->second.value)) {
            if (yearSkipNote) {
                std::string v = it->second.value;           // 截断,防脏值超长撑爆 note
                if (v.size() > 32) v = v.substr(0, 32) + "…";
                *yearSkipNote = "year 值非合理年份(\"" + v + "\"),已跳过";
            }
            continue;
        }
        WbField f;
        f.field = order[i];
        f.key   = keys[i];
        f.value = it->second.value;
        o.push_back(std::move(f));
    }
    return o;
}

// ---------------------------------------------------------------------------
// 纯解析内嵌封面保护门（不启动 ffmpeg，直接解析 FLAC/MP3 容器头部）
// ---------------------------------------------------------------------------

// 纯字节判定：解析内存中的容器头部字节链。
// 支持格式：.flac（解析 fLaC 元数据块链中的 PICTURE 块）、.mp3（解析 ID3v2 标签中的 APIC 帧）。
// 其它格式（ogg/opus/m4a/wav/wma/wv 等）暂不判，保持「未知不拦」（返回 false）。
// 检测到明确畸形（空 MIME、MIME/数据长度越界、缺少必要字段等）返回 true，并在 *reason 中回填说明。
inline bool writeback_detect_picture_malformed_bytes(
    const unsigned char* data, const size_t size,
    const std::string& ext, std::string* reason = nullptr) {
    if (!data || size < 4) return false;

    // 1. FLAC 容器头部 PICTURE 块解析
    if (ext == ".flac") {
        if (size < 4 || data[0] != 'f' || data[1] != 'L' || data[2] != 'a' || data[3] != 'C') {
            return false;
        }
        size_t pos = 4;
        bool isLast = false;
        while (pos + 4 <= size && !isLast) {
            const unsigned char h0 = data[pos];
            isLast = (h0 & 0x80) != 0;
            const int blockType = h0 & 0x7F;
            const uint32_t blockLen = ((uint32_t)data[pos + 1] << 16) |
                                      ((uint32_t)data[pos + 2] << 8)  |
                                      (uint32_t)data[pos + 3];
            const size_t dataStart = pos + 4;
            const size_t nextPos = dataStart + (size_t)blockLen;

            if (blockType == 6) { // PICTURE
                if (dataStart > size || nextPos > size) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(块长度越界超文件大小),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                if (blockLen < 8) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(块长度不足 8 字节),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                const unsigned char* p = data + dataStart;
                const uint32_t mimeLen = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                                         ((uint32_t)p[6] << 8)  | (uint32_t)p[7];
                if (mimeLen == 0) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(MIME 字符串为空),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                if ((size_t)mimeLen > blockLen - 8) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(MIME 字符串长度越界),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                for (size_t i = 0; i < mimeLen; ++i) {
                    const unsigned char c = p[8 + i];
                    if (c < 0x20 || c > 0x7E) {
                        if (reason) *reason = "源文件的 PICTURE 块畸形(MIME 字符串包含非法字符),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                        return true;
                    }
                }
                size_t cur = 8 + (size_t)mimeLen;
                if (cur + 4 > blockLen) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(缺少描述长度字段),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                const uint32_t descLen = ((uint32_t)p[cur] << 24) | ((uint32_t)p[cur + 1] << 16) |
                                         ((uint32_t)p[cur + 2] << 8) | (uint32_t)p[cur + 3];
                cur += 4;
                if ((size_t)descLen > blockLen - cur) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(描述字符串长度越界),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                cur += (size_t)descLen;
                if (cur + 16 + 4 > blockLen) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(缺少图片元信息或数据长度字段),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                cur += 16; // width (4) + height (4) + depth (4) + colors (4)
                const uint32_t dataLen = ((uint32_t)p[cur] << 24) | ((uint32_t)p[cur + 1] << 16) |
                                         ((uint32_t)p[cur + 2] << 8) | (uint32_t)p[cur + 3];
                cur += 4;
                if (dataLen == 0) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(图片数据长度为 0),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                if ((size_t)dataLen > blockLen - cur) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(图片数据长度越界),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
            }
            pos = nextPos;
        }
        return false;
    }

    // 2. MP3 ID3v2 APIC 帧解析
    if (ext == ".mp3") {
        if (size < 10 || data[0] != 'I' || data[1] != 'D' || data[2] != '3') return false;
        const unsigned char ver = data[3];
        if (ver < 2 || ver > 4) return false;
        const unsigned char flags = data[5];
        const bool hasExt = (flags & 0x40) != 0;
        for (int i = 6; i < 10; ++i) {
            if (data[i] & 0x80) return false; // 非法 syncsafe
        }
        const uint32_t tagSize = ((uint32_t)(data[6] & 0x7F) << 21) |
                                 ((uint32_t)(data[7] & 0x7F) << 14) |
                                 ((uint32_t)(data[8] & 0x7F) << 7)  |
                                 (uint32_t)(data[9] & 0x7F);
        size_t tagLimit = 10 + (size_t)tagSize;
        if (tagLimit > size) tagLimit = size;
        size_t pos = 10;
        if (hasExt) {
            if (ver == 3) {
                if (pos + 4 <= tagLimit) {
                    const uint32_t extSize = ((uint32_t)data[pos] << 24) | ((uint32_t)data[pos + 1] << 16) |
                                             ((uint32_t)data[pos + 2] << 8) | (uint32_t)data[pos + 3];
                    pos += 4 + (size_t)extSize;
                }
            } else if (ver == 4) {
                if (pos + 4 <= tagLimit) {
                    const uint32_t extSize = ((uint32_t)(data[pos] & 0x7F) << 21) |
                                             ((uint32_t)(data[pos + 1] & 0x7F) << 14) |
                                             ((uint32_t)(data[pos + 2] & 0x7F) << 7) |
                                             (uint32_t)(data[pos + 3] & 0x7F);
                    pos += (size_t)extSize;
                }
            }
        }
        while (pos + 10 <= tagLimit) {
            if (data[pos] == 0 && data[pos + 1] == 0) break; // 遭遇 padding，退出
            const bool isApic = (data[pos] == 'A' && data[pos + 1] == 'P' &&
                                 data[pos + 2] == 'I' && data[pos + 3] == 'C');
            uint32_t fsize = 0;
            if (ver == 4) {
                if ((data[pos + 4] & 0x80) || (data[pos + 5] & 0x80) ||
                    (data[pos + 6] & 0x80) || (data[pos + 7] & 0x80)) {
                    if (isApic) {
                        if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC 帧长编码非法),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                        return true;
                    }
                    break;
                }
                fsize = ((uint32_t)(data[pos + 4] & 0x7F) << 21) |
                        ((uint32_t)(data[pos + 5] & 0x7F) << 14) |
                        ((uint32_t)(data[pos + 6] & 0x7F) << 7)  |
                        (uint32_t)(data[pos + 7] & 0x7F);
            } else {
                fsize = ((uint32_t)data[pos + 4] << 24) | ((uint32_t)data[pos + 5] << 16) |
                        ((uint32_t)data[pos + 6] << 8)  | (uint32_t)data[pos + 7];
            }
            const size_t frameDataStart = pos + 10;
            const size_t nextPos = frameDataStart + (size_t)fsize;
            if (isApic) {
                if (frameDataStart > tagLimit || nextPos > 10 + (size_t)tagSize || (size >= 10 + (size_t)tagSize && nextPos > size)) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC 帧长越界),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                if (fsize < 4) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC 帧数据过短),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                const unsigned char* fp = data + frameDataStart;
                const unsigned char enc = fp[0];
                if (enc > 3) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC 编码字段非法),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                size_t mimeEnd = 1;
                while (mimeEnd < (size_t)fsize && fp[mimeEnd] != 0) ++mimeEnd;
                if (mimeEnd >= (size_t)fsize) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC MIME 缺少终止符),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                const size_t mimeLen = mimeEnd - 1;
                if (mimeLen == 0) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC MIME 字符串为空),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                for (size_t mi = 1; mi < mimeEnd; ++mi) {
                    if (fp[mi] < 0x20 || fp[mi] > 0x7E) {
                        if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC MIME 包含非法字符),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                        return true;
                    }
                }
                if (mimeEnd + 1 >= (size_t)fsize) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC 缺少图片类型),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
                const size_t descStart = mimeEnd + 2;
                size_t descEnd = descStart;
                if (enc == 0 || enc == 3) {
                    while (descEnd < (size_t)fsize && fp[descEnd] != 0) ++descEnd;
                    if (descEnd >= (size_t)fsize) {
                        if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC 描述缺少终止符),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                        return true;
                    }
                    descEnd += 1;
                } else {
                    while (descEnd + 1 < (size_t)fsize && !(fp[descEnd] == 0 && fp[descEnd + 1] == 0 && ((descEnd - descStart) % 2 == 0))) {
                        ++descEnd;
                    }
                    if (descEnd + 1 >= (size_t)fsize) {
                        if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC 描述缺少双字节终止符),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                        return true;
                    }
                    descEnd += 2;
                }
                if (descEnd >= (size_t)fsize) {
                    if (reason) *reason = "源文件的 PICTURE 块畸形(ID3v2 APIC 图片数据为空),remux 会静默丢失该封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                    return true;
                }
            }
            pos = nextPos;
        }
        return false;
    }

    return false;
}

// 纯文件入口：读取文件头部字节（探测 FLAC/MP3 容器内嵌封面是否畸形，不启动 ffmpeg）
inline bool writeback_detect_picture_malformed(const std::string& file, std::string* reason = nullptr) {
    const std::string ext = rta::platform::lower_extension(file);
    if (ext != ".flac" && ext != ".mp3") return false;

    std::ifstream f(meta_path_of(file), std::ios::binary);
    if (!f) return false;

    f.seekg(0, std::ios::end);
    const auto endPos = f.tellg();
    if (endPos <= 0) return false;
    const size_t fileSize = (size_t)endPos;
    f.seekg(0, std::ios::beg);

    // 默认读取前 1MB 探测；若封面更大则按需扩容
    size_t toRead = std::min<size_t>(fileSize, 1024 * 1024);
    std::vector<unsigned char> buf(toRead);
    f.read((char*)buf.data(), (std::streamsize)toRead);
    size_t nRead = (size_t)f.gcount();
    buf.resize(nRead);

    // 若是 FLAC，检查是否有 PICTURE 块超出了当前的 1MB 缓冲
    if (ext == ".flac" && buf.size() >= 4 && buf[0] == 'f' && buf[1] == 'L' && buf[2] == 'a' && buf[3] == 'C') {
        size_t pos = 4;
        bool isLast = false;
        while (pos + 4 <= buf.size() && !isLast) {
            isLast = (buf[pos] & 0x80) != 0;
            const int blockType = buf[pos] & 0x7F;
            const uint32_t blockLen = ((uint32_t)buf[pos + 1] << 16) |
                                      ((uint32_t)buf[pos + 2] << 8)  |
                                      (uint32_t)buf[pos + 3];
            const size_t nextPos = pos + 4 + (size_t)blockLen;
            if (blockType == 6 && nextPos > buf.size()) {
                if (nextPos <= fileSize && nextPos <= 50 * 1024 * 1024) {
                    buf.resize(nextPos);
                    f.seekg((std::streamoff)nRead);
                    f.read((char*)buf.data() + nRead, (std::streamsize)(nextPos - nRead));
                    nRead += (size_t)f.gcount();
                    buf.resize(nRead);
                }
                break;
            }
            pos = nextPos;
        }
    } else if (ext == ".mp3" && buf.size() >= 10 && buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        const uint32_t tagSize = ((uint32_t)(buf[6] & 0x7F) << 21) |
                                 ((uint32_t)(buf[7] & 0x7F) << 14) |
                                 ((uint32_t)(buf[8] & 0x7F) << 7)  |
                                 (uint32_t)(buf[9] & 0x7F);
        const size_t needed = 10 + (size_t)tagSize;
        if (needed > buf.size() && needed <= fileSize && needed <= 50 * 1024 * 1024) {
            buf.resize(needed);
            f.seekg((std::streamoff)nRead);
            f.read((char*)buf.data() + nRead, (std::streamsize)(needed - nRead));
            nRead += (size_t)f.gcount();
            buf.resize(nRead);
        }
    }

    return writeback_detect_picture_malformed_bytes(buf.data(), buf.size(), ext, reason);
}

// ---------------------------------------------------------------------------
// 命令行拼装与进程执行(经 CreateProcessW 宽字符;ffmpeg 输出只用于错误消息)
// ---------------------------------------------------------------------------

// UTF-8 参数 → 带引号的宽命令行片段。MSVCRT 解析规则:值内 " → \" ;
// 紧邻引号的反斜杠翻倍(含结尾!结尾不翻倍会把闭引号转义掉 —— 2026-10-22 探针实测踩到)。
// 实测(同日探针 + 写回门禁):中文、双引号、\n、\r\n、\=、结尾反斜杠全部无损往返。
// 返回值从 std::wstring 改成 UTF-8 std::string：命令行整串在平台层内部才转宽字符。
// 转义算法逐字符不变，且对 UTF-8 字节逐字节仍然等价 —— 多字节序列的续字节都 >= 0x80，
// 不可能等于 '"' 或 '\\'，所以"按 wchar_t 遍历"与"按 UTF-8 字节遍历"结果一致。
inline std::string wb_quoted(const std::string& u8) {
    std::string o = "\"";
    int bs = 0;
    for (const char c : u8) {
        if (c == '\\') { ++bs; o += c; continue; }
        if (c == '"')  { o.append((size_t)bs + 1, '\\'); o += c; }
        else o += c;
        bs = 0;
    }
    o.append((size_t)bs, '\\');
    o += '"';
    return o;
}

// ffmpeg 日志压成单行、截断(仿 audio_loader.h read_via_ffmpeg 的做法,进错误消息不吞不瞒)
inline std::string wb_log_tail(const std::string& log, const size_t maxn = 300) {
    std::string t;
    t.reserve(log.size());
    for (const char c : log) {
        if (c == '\r' || c == '\n') { if (!t.empty() && t.back() != ' ') t.push_back(' '); }
        else t.push_back(c);
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.size() > maxn) t = t.substr(0, maxn) + "...";
    return t;
}

// UTF-8 安全截断(回退到序列边界,不切坏中文;末尾加 …)
inline std::string wb_preview(const std::string& s, const size_t n = 32) {
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) --k;
    return s.substr(0, k) + "…";
}

// ---------------------------------------------------------------------------
// 内部三步:remux / 解码 / 比较(独立成 inline 函数,tests/test_writeback.cpp 可函数级实测)
// ---------------------------------------------------------------------------

// remux:ffmpeg -v error -y -i src -map 0 -c copy -f <muxer>[mp3 专选项] -metadata K=V… dst
// 返回 false 时 err 带 ffmpeg 输出尾部。dst 的扩展名是 .rtatmp,所以 -f 必须显式。
inline bool writeback_remux(const std::string& ffmpeg, const std::string& src, const std::string& dst,
                            const std::string& muxer, const bool isMp3,
                            const std::vector<WbField>& kvs, std::string& err) {
    std::string cmd = "\"" + ffmpeg + "\" -v error -y -i " + wb_quoted(src) +
                      " -map 0 -c copy -f " + wb_quoted(muxer);
    if (isMp3) cmd += " -write_id3v2 1 -id3v2_version 4";
    for (const WbField& f : kvs)
        cmd += " -metadata " + wb_quoted(std::string(f.key) + "=" + f.value);
    cmd += " " + wb_quoted(dst);
    const rta::platform::ProcessResult pr = rta::platform::run_hidden(cmd, true);
    const std::string& log = pr.output;
    const int rc = pr.exit_code;
    if (rc != 0) {
        err = "ffmpeg remux 失败(退出码 " + std::to_string(rc) + ")" +
              (log.empty() ? "" : ":" + wb_log_tail(log));
        return false;
    }
    return true;
}

// 解码为同参数 PCM WAV(pcm_s32le)。-map_metadata -1:新旧文件标签不同,不隔离会污染
// 逐字节比较;-vn:去掉封面流;-map 0:a:0:只取首音频流(多音频流容器 -map 0:a 会因
// wav 单流限制直接失败,音乐曲目取首流即是要比较的对象)。两份解码参数完全一致。
inline bool writeback_decode_pcm(const std::string& ffmpeg, const std::string& in,
                                 const std::string& outWav, std::string& err) {
    const std::string cmd = "\"" + ffmpeg + "\" -v error -y -i " + wb_quoted(in) +
                            " -map 0:a:0 -vn -map_metadata -1 -f wav -c:a pcm_s32le " + wb_quoted(outWav);
    const rta::platform::ProcessResult pr = rta::platform::run_hidden(cmd, true);
    const std::string& log = pr.output;
    const int rc = pr.exit_code;
    if (rc != 0) {
        err = "ffmpeg 解码失败(输入=" + in + ",退出码 " + std::to_string(rc) + ")" +
              (log.empty() ? "" : ":" + wb_log_tail(log));
        return false;
    }
    // HF-03 教训:退出码 0 不代表产物可用,检查存在与最小尺寸
    std::error_code ec;
    const uintmax_t sz = fsx::file_size(meta_path_of(outWav), ec);
    if (ec || sz < 44) {
        err = "ffmpeg 解码产物异常(输入=" + in + ",WAV " +
              (ec ? "不可读" : std::to_string((unsigned long long)sz) + " 字节") + ")";
        return false;
    }
    return true;
}

// 逐字节比较(含长度)。一致返回 true;不一致返回 false 且 diff 写明长度差/首个差异偏移。
inline bool writeback_pcm_identical(const std::string& wavA, const std::string& wavB, std::string& diff) {
    std::vector<unsigned char> a, b;
    std::ifstream fa(meta_path_of(wavA), std::ios::binary);
    if (!fa) { diff = "读不了 A=" + wavA; return false; }
    a.assign(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>());
    std::ifstream fb(meta_path_of(wavB), std::ios::binary);
    if (!fb) { diff = "读不了 B=" + wavB; return false; }
    b.assign(std::istreambuf_iterator<char>(fb), std::istreambuf_iterator<char>());
    if (a.size() != b.size()) {
        diff = "长度不同(A=" + std::to_string(a.size()) + "B,B=" + std::to_string(b.size()) + "B)";
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            char b1[8], b2[8];
            std::snprintf(b1, sizeof(b1), "0x%02x", a[i]);
            std::snprintf(b2, sizeof(b2), "0x%02x", b[i]);
            diff = "第 " + std::to_string(i) + " 字节不同(A=" + b1 + ",B=" + b2 + "),共 " +
                   std::to_string(a.size()) + "B";
            return false;
        }
    }
    diff = "逐字节相同(" + std::to_string(a.size()) + "B)";
    return true;
}

// ---------------------------------------------------------------------------
// writeback_file / writeback_library
// ---------------------------------------------------------------------------

struct WritebackOpts {
    bool dryRun = true;   // 默认 dry-run:全流程含 PCM 校验,但不替换原文件
    bool backup = true;   // 替换前把原文件移入 <store>/backups/<hash>.<ext>
};

struct WritebackResult {
    bool ok = false;                          // 全流程走完(含 dry-run)
    bool skipped = false;                     // 不适用(DSD/未知扩展/无可写字段),不是失败
    bool dryRun = false;
    bool pcmIdentical = false;
    std::string file, err, note;              // note:写了哪些键/跳过原因;err:失败原因
    std::vector<std::string> writtenFields;   // 本次实际写入的字段名(TrackMeta 字段名)
};

inline WritebackResult writeback_file(const std::string& file, const TrackMeta& tm,
                                      const WritebackOpts& opts) {
    WritebackResult r;
    r.file = file;

    // ① 扩展名门(安全红线③:绝不写 DSD 与不可识别扩展)
    const std::string ext = rta::platform::lower_extension(file);
    if (ext == ".dsf" || ext == ".dff") {
        r.skipped = true;
        r.note = "DSD(" + ext + ")按安全红线绝不写回";
        return r;
    }
    if (ext == ".ape") {
        r.skipped = true;
        r.note = "ffmpeg 无 APE muxer(实测 -muxers),无法无损 remux,跳过(不冒有损转码之险)";
        return r;
    }
    const char* muxer = writeback_muxer(ext);
    if (!muxer) {
        r.skipped = true;
        r.note = "不支持的扩展名 \"" + ext + "\"(支持 flac/mp3/ogg/opus/m4a/wma/wav/wv)";
        return r;
    }
    const bool vorbis = writeback_vorbis_family(ext);

    // ② 收集非空字段(year 脏值在 collect 内被滤除,跳过说明带回并入 note)
    std::string yearSkip;
    const std::vector<WbField> kvs = writeback_collect(tm, vorbis, &yearSkip);
    if (kvs.empty()) {
        r.skipped = true;
        r.note = yearSkip.empty()
                     ? "无可写字段(MetaStore 12 个常规字段全空)"
                     : yearSkip + ";其余常规字段全空,无可写字段";
        return r;
    }
    for (const WbField& f : kvs) r.writtenFields.push_back(f.field);

    // ③ 工具与源文件
    const std::string ffmpeg = rta::platform::find_executable("ffmpeg");
    if (ffmpeg.empty()) {
        r.err = "找不到 ffmpeg(find_tool):无法 remux 与 PCM 校验。把 ffmpeg.exe 放到程序同目录或加入 PATH";
        return r;
    }
    std::error_code ec;
    if (!fsx::exists(meta_path_of(file), ec) || ec) {
        r.err = "源文件不存在:" + file;
        return r;
    }

    // ③b 内嵌封面保护门(纯解析门 + ffprobe 探测双保险)
    {
        std::string picReason;
        if (writeback_detect_picture_malformed(file, &picReason)) {
            r.skipped = true;
            r.note = picReason;
            return r;
        }
        const std::string ffprobe = rta::platform::find_executable("ffprobe");
        if (!ffprobe.empty()) {
            const std::string cmd = "\"" + ffprobe + "\" -hide_banner " + wb_quoted(file);
            const std::string warn = rta::platform::run_hidden(cmd, true).output;
            if (warn.find("attached picture") != std::string::npos) {
                r.skipped = true;
                r.note = "源文件的 PICTURE 块畸形(ffmpeg 读不出内嵌封面),remux 会静默丢失该"
                         "封面块;已跳过写回。请先重建内嵌封面(重新嵌入图片)后再写回";
                return r;
            }
        }
    }

    // ④ remux 到同目录 .rtatmp 临时文件(先清掉上次崩溃可能留下的残留)
    const std::string temp = file + ".rtatmp";
    rta::platform::remove_file(meta_path_of(temp));
    std::string err;
    if (!writeback_remux(ffmpeg, file, temp, muxer, ext == ".mp3", kvs, err)) {
        rta::platform::remove_file(meta_path_of(temp));
        r.err = err;
        return r;
    }

    // ⑤ PCM 门禁:两份同参数解码(pcm_s32le)逐字节比较(含长度)
    // temp_dir_utf8() 返回带尾分隔符的临时目录（语义 = GetTempPathW），下面直接后缀拼接
    const std::string tmpDir = rta::platform::temp_dir_utf8();
    const std::string tag = MetaStore::hashOf(file);
    const std::string pcmOld = tmpDir + "rta_wb_" + tag + "_old.wav";
    const std::string pcmNew = tmpDir + "rta_wb_" + tag + "_new.wav";
    auto cleanup = [&]() {
        rta::platform::remove_file(meta_path_of(temp));
        rta::platform::remove_file(meta_path_of(pcmOld));
        rta::platform::remove_file(meta_path_of(pcmNew));
    };
    if (!writeback_decode_pcm(ffmpeg, file, pcmOld, err) ||
        !writeback_decode_pcm(ffmpeg, temp, pcmNew, err)) {
        cleanup();
        r.err = "PCM 校验中断:" + err;
        return r;
    }
    std::string diff;
    r.pcmIdentical = writeback_pcm_identical(pcmOld, pcmNew, diff);
    if (!r.pcmIdentical) {
        cleanup();
        r.err = "PCM 校验失败:写回前后解码不一致(" + diff + ");已删除临时文件,原文件未动";
        return r;
    }
    rta::platform::remove_file(meta_path_of(pcmOld));
    rta::platform::remove_file(meta_path_of(pcmNew));

    // ⑥ 字段清单进 note
    std::string listed;
    for (const WbField& f : kvs) {
        if (!listed.empty()) listed += ", ";
        listed += std::string(f.field) + "=\"" + wb_preview(f.value) + "\"";
    }

    if (opts.dryRun) {
        cleanup();                              // dry-run:到此为止,不留任何文件变更
        r.ok = true;
        r.dryRun = true;
        r.note = "dry-run:PCM 校验通过(" + diff + ");将写 " + std::to_string(kvs.size()) +
                 " 个字段: " + listed + (yearSkip.empty() ? "" : ";" + yearSkip);
        return r;
    }

    // ⑦ 备份:原文件移入 <store>/backups/<hash>.<ext>(同盘 MoveFileEx 原子;跨盘退化为复制)
    std::string backupNote;
    bool movedNotCopied = false;
    fsx::path backupUsed;                       // 实际备份位置(恢复防御要用真实路径)
    if (opts.backup) {
        const fsx::path backupDir = meta_path_of(MetaStore::instance().baseDir()) / "backups";
        fsx::create_directories(backupDir, ec);
        const std::string hash = MetaStore::hashOf(file);
        fsx::path backup = backupDir / (hash + ext);
        for (int n = 2; n < 1000; ++n) {        // 同一文件多次写回:不覆盖最早的原始备份
            std::error_code ec2;
            if (!fsx::exists(backup, ec2) || ec2) break;
            backup = backupDir / (hash + ".r" + std::to_string(n) + ext);
        }
        backupUsed = backup;
        const fsx::path srcP = meta_path_of(file);
        if (rta::platform::move_file(srcP, backup, false)) {
            movedNotCopied = true;
        } else {
            // 跨盘(move 不支持)或其它失败:退化为复制;原文件仍在,替换用 REPLACE_EXISTING
            const int gle = rta::platform::last_error_code();
            if (!rta::platform::copy_file(srcP, backup, false)) {
                cleanup();
                r.err = "备份失败(move 与 copy 均失败,GetLastError=" + std::to_string(gle) +
                        ");原文件未动";
                return r;
            }
        }
        backupNote = "原文件 → " + meta_string_of(backup) + (movedNotCopied ? "(移入)" : "(跨盘复制)");
    } else {
        backupNote = "无备份(--no-backup)";
    }

    // ⑧ 替换:临时文件 rename 成原路径;失败则把原文件从备份搬回(防御)
    if (!rta::platform::move_file(meta_path_of(temp), meta_path_of(file), true)) {
        const int gle = rta::platform::last_error_code();
        std::string restore = "未恢复";
        if (movedNotCopied) {
            const bool ok = rta::platform::move_file(backupUsed, meta_path_of(file), true);
            restore = ok ? "已从备份搬回" : "搬回失败(备份在 " + meta_string_of(backupUsed) + ")";
        } else if (opts.backup) {
            // 跨盘复制路径:原文件还在,只需删掉复制物与临时件
            rta::platform::remove_file(backupUsed);
            restore = "原文件未被动过(跨盘复制路径)";
        }
        cleanup();
        r.err = "替换失败(GetLastError=" + std::to_string(gle) + ");" + restore;
        return r;
    }

    r.ok = true;
    r.note = "已写 " + std::to_string(kvs.size()) + " 个字段: " + listed + ";PCM 逐字节一致;" +
             backupNote + (yearSkip.empty() ? "" : ";" + yearSkip);
    return r;
}

// ---------------------------------------------------------------------------
// 整库批量:遍历 MetaStore 记录目录(<base>/tracks/*.json),失败队列落盘
// ---------------------------------------------------------------------------

struct BatchResult {
    int total = 0, written = 0, skipped = 0, failed = 0;
    std::vector<std::string> failures;                                  // "file | err"(CLI 打印用)
    std::vector<std::pair<std::string, std::string>> failureList;       // 结构化(持久化用)
};

// 从 <base>/tracks/<hash>.json 读回 TrackMeta(镜像 store.h load 的解析逻辑,
// 但入口是 json 文件本身 —— hash 即文件名,path 存在 JSON 里)
inline bool writeback_load_store_record(const fsx::path& jsonFile, TrackMeta& out) {
    std::error_code ec;
    if (!fsx::exists(jsonFile, ec) || ec) return false;
    std::ifstream f(jsonFile, std::ios::binary);
    if (!f) return false;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.empty()) return false;
    const rta::net::Json j = rta::net::json_parse(s);
    if (j.type != rta::net::Json::Obj) return false;
    if (!j.has("fields") || j["fields"].type != rta::net::Json::Obj) return false;
    TrackMeta tm;
    tm.path         = j["path"].s();
    tm.coverRelPath = j["cover"].s();
    for (const auto& kv : j["fields"].obj) {
        const rta::net::Json& fv = kv.second;
        if (fv.type != rta::net::Json::Obj) continue;
        MetaValue v;
        v.value      = fv["value"].s();
        v.source     = fv["source"].s();
        v.ts         = (long long)fv["ts"].n();
        v.confidence = (float)fv["confidence"].n();
        tm.fields[kv.first] = std::move(v);
    }
    if (tm.path.empty()) return false;
    out = std::move(tm);
    return true;
}

// 失败队列原子写(仿 store.h save:tmp + rename);无失败也写(清掉旧队列,UI 才能反映现状)
inline void writeback_write_failures(const std::string& baseDir, const BatchResult& br) {
    std::ostringstream o;
    o << "{\"generated\":" << (long long)std::time(nullptr)
      << ",\"total\":" << br.total
      << ",\"written\":" << br.written
      << ",\"skipped\":" << br.skipped
      << ",\"failed\":" << br.failed
      << ",\"failures\":[";
    bool first = true;
    for (const auto& fe : br.failureList) {
        if (!first) o << ',';
        first = false;
        o << "{\"file\":\"" << meta_jesc(fe.first) << "\",\"err\":\"" << meta_jesc(fe.second) << "\"}";
    }
    o << "]}";
    const fsx::path dir = meta_path_of(baseDir);
    std::error_code ec;
    fsx::create_directories(dir, ec);
    fsx::path file = dir / "writeback-failures.json";
    fsx::path tmp  = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const std::string s = o.str();
        f.write(s.data(), (std::streamsize)s.size());
        if (!f) return;
    }
    fsx::rename(tmp, file, ec);
}

// 带进度回调的重载(P2 Web 接入 /api/writeback/status 轮询用):每处理完一条记录
// 以「累计中的 BatchResult + done/total」回调一次 —— 计数与 failureList 都是实时的。
// 回调在 writeback_library 调用线程上同步执行;回调里不得再进 writeback_library。
inline BatchResult writeback_library(
    const WritebackOpts& opts, std::string* errOut,
    const std::function<void(const BatchResult& br, int done, int total)>& onProgress) {
    BatchResult br;
    if (errOut) errOut->clear();
    const std::string baseDir = MetaStore::instance().baseDir();
    const fsx::path tracksDir = meta_path_of(baseDir) / "tracks";
    std::error_code ec;
    if (!fsx::exists(tracksDir, ec) || ec) {
        if (errOut) *errOut = "MetaStore 记录目录不存在:" + baseDir + "/tracks(先 enrich 或 store set)";
        return br;
    }
    std::vector<fsx::path> jsons;
    for (fsx::directory_iterator it(tracksDir, ec), end; !ec && it != end; it.increment(ec)) {
        const fsx::path p = it->path();
        const std::string name = meta_string_of(p.filename());
        std::error_code ec3;
        if (!it->is_regular_file(ec3) || ec3) continue;
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        jsons.push_back(p);
    }
    std::sort(jsons.begin(), jsons.end());      // 确定性顺序

    int done = 0;
    for (const fsx::path& jp : jsons) {
        ++br.total;
        TrackMeta tm;
        if (!writeback_load_store_record(jp, tm)) {
            ++br.failed;
            const std::string f = meta_string_of(jp.filename()) + " (记录损坏/缺 path)";
            br.failureList.push_back({ f, "记录解析失败" });
            br.failures.push_back(f + " | 记录解析失败");
        } else {
            const WritebackResult r = writeback_file(tm.path, tm, opts);
            if (r.skipped)      { ++br.skipped; }               // skip 不是失败
            else if (!r.ok) {
                ++br.failed;
                br.failureList.push_back({ tm.path, r.err });
                br.failures.push_back(tm.path + " | " + r.err);
            }
            else ++br.written;                   // 含 dry-run(整个库跑通校验但不落盘)
        }
        ++done;
        if (onProgress) onProgress(br, done, (int)jsons.size());
    }
    writeback_write_failures(baseDir, br);
    return br;
}

// 原有两参签名不变:等价于无进度回调。
inline BatchResult writeback_library(const WritebackOpts& opts, std::string* errOut) {
    return writeback_library(opts, errOut, {});
}

} // namespace rta::meta
